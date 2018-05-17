// vtcomposite
#include "vtcomposite.hpp"
#include "module_utils.hpp"
#include "zxy_math.hpp"
#include "extract_geometry.hpp"
#include "zoom_coordinates.hpp"
#include "feature_builder.hpp"
// gzip-hpp
#include <gzip/decompress.hpp>
#include <gzip/utils.hpp>
// vtzero
#include <vtzero/builder.hpp>
#include <vtzero/vector_tile.hpp>
// geometry.hpp
#include <mapbox/geometry/geometry.hpp>
#include <mapbox/geometry/box.hpp>
// stl
#include <algorithm>

namespace vtile {

struct TileObject
{
    TileObject(std::uint32_t z0,
               std::uint32_t x0,
               std::uint32_t y0,
               v8::Local<v8::Object> buffer)
        : z{z0},
          x{x0},
          y{y0},
          data{node::Buffer::Data(buffer), node::Buffer::Length(buffer)},
          buffer_ref{}
    {
        buffer_ref.Reset(buffer.As<v8::Object>());
    }

    ~TileObject()
    {
        buffer_ref.Reset();
    }

    // non-copyable
    TileObject(TileObject const&) = delete;
    TileObject& operator=(TileObject const&) = delete;

    // non-movable
    TileObject(TileObject&&) = delete;
    TileObject& operator=(TileObject&&) = delete;

    std::uint32_t z;
    std::uint32_t x;
    std::uint32_t y;
    vtzero::data_view data;
    Nan::Persistent<v8::Object> buffer_ref;
};

struct BatonType
{
    explicit BatonType(std::uint32_t num_tiles)
    {
        tiles.reserve(num_tiles);
    }

    // non-copyable
    BatonType(BatonType const&) = delete;
    BatonType& operator=(BatonType const&) = delete;

    // non-movable
    BatonType(BatonType&&) = delete;
    BatonType& operator=(BatonType&&) = delete;

    // members
    std::vector<std::unique_ptr<TileObject>> tiles{};
    std::uint32_t z{};
    std::uint32_t x{};
    std::uint32_t y{};
};

struct CompositeWorker : Nan::AsyncWorker
{
    using Base = Nan::AsyncWorker;

    CompositeWorker(std::unique_ptr<BatonType> baton_data, Nan::Callback* cb)
        : Base{cb},
          baton_data_{std::move(baton_data)},
          output_buffer_{std::make_unique<std::string>()} {}

    void Execute() override
    {
        try
        {
            gzip::Decompressor decompressor;
            vtzero::tile_builder builder;
            std::vector<std::string> names;
            vtzero::data_view tile_view{};

            std::uint32_t const target_z = baton_data_->z;
            std::uint32_t const target_x = baton_data_->x;
            std::uint32_t const target_y = baton_data_->y;

            for (auto const& tile_obj : baton_data_->tiles)
            {
                if (vtile::within_target(*tile_obj, target_z, target_x, target_y))
                {
                    std::vector<char> buffer;
                    if (gzip::is_compressed(tile_obj->data.data(), tile_obj->data.size()))
                    {
                        decompressor.decompress(buffer, tile_obj->data.data(), tile_obj->data.size());
                        tile_view = protozero::data_view{buffer.data(), buffer.size()};
                    }
                    else
                    {
                        tile_view = tile_obj->data;
                    }

                    int zoom_factor = 1 << (target_z - tile_obj->z);
                    vtzero::vector_tile tile{tile_view};
                    while (auto layer = tile.next_layer())
                    {
                        std::string name{layer.name()};
                        if (std::find(std::begin(names), std::end(names), name) == std::end(names))
                        {
                            names.push_back(name);
                            if (zoom_factor == 1)
                            {
                                builder.add_existing_layer(layer);
                            }
                            else
                            {
                                vtzero::layer_builder layer_builder{builder, layer};
                                layer.for_each_feature([&](vtzero::feature const& feature) {
                                    auto geom = vtile::extract_geometry<std::int32_t>(feature);
                                    // scale by zoom_factor
                                    mapbox::geometry::for_each_point(geom,
                                                                     vtile::detail::zoom_coordinates<mapbox::geometry::point<std::int32_t>>(zoom_factor));
                                    int const tile_size = 4096u;
                                    int dx, dy;
                                    std::tie(dx, dy) = vtile::displacement(zoom_factor, tile_size, target_z, target_x, target_y);
                                    mapbox::geometry::box<std::int32_t> bbox{{dx, dy}, {dx + tile_size, dy + tile_size}};
                                    mapbox::util::apply_visitor(vtile::feature_builder<std::int32_t>{layer_builder, bbox, feature}, geom);
                                    return true;
                                });
                            }
                        }
                    }
                }
                else
                {
                    std::cerr << "Invalid tile composite request" << std::endl;
                }
            }
            std::string& tile_buffer = *output_buffer_.get();
            builder.serialize(tile_buffer);
        }
        catch (std::exception const& e)
        {
            SetErrorMessage(e.what());
        }
    }

    void HandleOKCallback() override
    {
        std::string& tile_buffer = *output_buffer_.get();
        Nan::HandleScope scope;
        const auto argc = 2u;
        v8::Local<v8::Value> argv[argc] = {
            Nan::Null(),
            Nan::NewBuffer(&tile_buffer[0],
                           static_cast<std::uint32_t>(tile_buffer.size()),
                           [](char*, void* hint) {
                               delete reinterpret_cast<std::string*>(hint);
                           },
                           output_buffer_.release())
                .ToLocalChecked()};

        // Static cast done here to avoid 'cppcoreguidelines-pro-bounds-array-to-pointer-decay' warning with clang-tidy
        callback->Call(argc, static_cast<v8::Local<v8::Value>*>(argv), async_resource);
    }

    std::unique_ptr<BatonType> const baton_data_;
    std::unique_ptr<std::string> output_buffer_;
};

NAN_METHOD(composite)
{
    // validate callback function
    v8::Local<v8::Value> callback_val = info[info.Length() - 1];
    if (!callback_val->IsFunction())
    {
        Nan::ThrowError("last argument must be a callback function");
        return;
    }

    v8::Local<v8::Function> callback = callback_val.As<v8::Function>();

    // validate tiles
    if (!info[0]->IsArray())
    {
        return utils::CallbackError("first arg 'tiles' must be an array of tile objects", callback);
    }

    v8::Local<v8::Array> tiles = info[0].As<v8::Array>();
    unsigned num_tiles = tiles->Length();

    if (num_tiles <= 0)
    {
        return utils::CallbackError("'tiles' array must be of length greater than 0", callback);
    }

    std::unique_ptr<BatonType> baton_data = std::make_unique<BatonType>(num_tiles);

    for (unsigned t = 0; t < num_tiles; ++t)
    {
        v8::Local<v8::Value> tile_val = tiles->Get(t);
        if (!tile_val->IsObject())
        {
            return utils::CallbackError("items in 'tiles' array must be objects", callback);
        }
        v8::Local<v8::Object> tile_obj = tile_val->ToObject();

        // check buffer value
        if (!tile_obj->Has(Nan::New("buffer").ToLocalChecked()))
        {
            return utils::CallbackError("item in 'tiles' array does not include a buffer value", callback);
        }
        v8::Local<v8::Value> buf_val = tile_obj->Get(Nan::New("buffer").ToLocalChecked());
        if (buf_val->IsNull() || buf_val->IsUndefined())
        {
            return utils::CallbackError("buffer value in 'tiles' array item is null or undefined", callback);
        }
        v8::Local<v8::Object> buffer = buf_val->ToObject();
        if (!node::Buffer::HasInstance(buffer))
        {
            return utils::CallbackError("buffer value in 'tiles' array item is not a true buffer", callback);
        }

        // z value
        if (!tile_obj->Has(Nan::New("z").ToLocalChecked()))
        {
            return utils::CallbackError("item in 'tiles' array does not include a 'z' value", callback);
        }
        v8::Local<v8::Value> z_val = tile_obj->Get(Nan::New("z").ToLocalChecked());
        if (!z_val->IsNumber())
        {
            return utils::CallbackError("'z' value in 'tiles' array item is not a number", callback);
        }
        std::int64_t z = z_val->IntegerValue();
        if (z < 0)
        {
            return utils::CallbackError("'z' value must not be less than zero", callback);
        }

        // x value
        if (!tile_obj->Has(Nan::New("x").ToLocalChecked()))
        {
            return utils::CallbackError("item in 'tiles' array does not include a 'x' value", callback);
        }
        v8::Local<v8::Value> x_val = tile_obj->Get(Nan::New("x").ToLocalChecked());
        if (!x_val->IsNumber())
        {
            return utils::CallbackError("'x' value in 'tiles' array item is not a number", callback);
        }
        std::int64_t x = x_val->IntegerValue();
        if (x < 0)
        {
            return utils::CallbackError("'x' value must not be less than zero", callback);
        }

        // y value
        if (!tile_obj->Has(Nan::New("y").ToLocalChecked()))
        {
            return utils::CallbackError("item in 'tiles' array does not include a 'y' value", callback);
        }
        v8::Local<v8::Value> y_val = tile_obj->Get(Nan::New("y").ToLocalChecked());
        if (!y_val->IsNumber())
        {
            return utils::CallbackError("'y' value in 'tiles' array item is not a number", callback);
        }
        std::int64_t y = y_val->IntegerValue();
        if (y < 0)
        {
            return utils::CallbackError("'y' value must not be less than zero", callback);
        }

        std::unique_ptr<TileObject> tile{new TileObject{static_cast<std::uint32_t>(z),
                                                        static_cast<std::uint32_t>(x),
                                                        static_cast<std::uint32_t>(y),
                                                        buffer}};
        baton_data->tiles.push_back(std::move(tile));
    }

    //validate zxy maprequest object
    if (!info[1]->IsObject())
    {
        return utils::CallbackError("'zxy_maprequest' must be an object", callback);
    }
    v8::Local<v8::Object> zxy_maprequest = v8::Local<v8::Object>::Cast(info[1]);

    // z value of map request object
    if (!zxy_maprequest->Has(Nan::New("z").ToLocalChecked()))
    {
        return utils::CallbackError("item in 'tiles' array does not include a 'z' value", callback);
    }
    v8::Local<v8::Value> z_val_maprequest = zxy_maprequest->Get(Nan::New("z").ToLocalChecked());
    if (!z_val_maprequest->IsNumber())
    {
        return utils::CallbackError("'z' value in 'tiles' array item is not a number", callback);
    }
    std::int64_t z_maprequest = z_val_maprequest->IntegerValue();
    if (z_maprequest < 0)
    {
        return utils::CallbackError("'z' value must not be less than zero", callback);
    }
    baton_data->z = static_cast<std::uint32_t>(z_maprequest);

    // x value of map request object
    if (!zxy_maprequest->Has(Nan::New("x").ToLocalChecked()))
    {
        return utils::CallbackError("item in 'tiles' array does not include a 'x' value", callback);
    }
    v8::Local<v8::Value> x_val_maprequest = zxy_maprequest->Get(Nan::New("x").ToLocalChecked());
    if (!x_val_maprequest->IsNumber())
    {
        return utils::CallbackError("'x' value in 'tiles' array item is not a number", callback);
    }
    std::int64_t x_maprequest = x_val_maprequest->IntegerValue();
    if (x_maprequest < 0)
    {
        return utils::CallbackError("'x' value must not be less than zero", callback);
    }

    baton_data->x = static_cast<std::uint32_t>(x_maprequest);

    // y value of maprequest object
    if (!zxy_maprequest->Has(Nan::New("y").ToLocalChecked()))
    {
        return utils::CallbackError("item in 'tiles' array does not include a 'y' value", callback);
    }
    v8::Local<v8::Value> y_val_maprequest = zxy_maprequest->Get(Nan::New("y").ToLocalChecked());
    if (!y_val_maprequest->IsNumber())
    {
        return utils::CallbackError("'y' value in 'tiles' array item is not a number", callback);
    }
    std::int64_t y_maprequest = y_val_maprequest->IntegerValue();
    if (y_maprequest < 0)
    {
        return utils::CallbackError("'y' value must not be less than zero", callback);
    }

    baton_data->y = static_cast<std::uint32_t>(y_maprequest);

    // enter the threadpool, then done in the callback function call the threadpool
    auto* worker = new CompositeWorker{std::move(baton_data), new Nan::Callback{callback}};
    Nan::AsyncQueueWorker(worker);
}

} // namespace vtile
