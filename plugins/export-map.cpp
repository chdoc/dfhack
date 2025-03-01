#include "Debug.h"
#include "Error.h"
#include "PluginManager.h"
#include "MiscUtils.h"

#include "modules/Maps.h"
#include "modules/Translation.h"

#include "df/world.h"
#include "df/map_block.h"
#include "df/world_data.h"
#include "df/region_map_entry.h"
#include "df/world_region.h"
#include "df/world_landmass.h"
#include "df/world_region_details.h"

#include "gdal/ogrsf_frmts.h"

#include <string>
#include <vector>
#include <chrono>

using std::string;
using std::vector;

using namespace DFHack;

DFHACK_PLUGIN("export-map");

REQUIRE_GLOBAL(world);

namespace DFHack {
    DBG_DECLARE(exportmap, log);
}

static command_result do_command(color_ostream &out, vector<string> &parameters);

DFhackCExport command_result plugin_init(color_ostream &out, std::vector <PluginCommand> &commands) {
    DEBUG(log,out).print("initializing %s\n", plugin_name);

    commands.push_back(PluginCommand(
        plugin_name,
        "Export the world map.",
        do_command));

    return CR_OK;
}

auto setGeometry(OGRFeature *feature, double x, double y, double dim) {
    auto poly = new OGRPolygon();
    auto boundary = new OGRLinearRing();
    y = -y; // in GIS negative y-coordinates mean further south
    boundary->addPoint(x,y);
    boundary->addPoint(x,y-dim);
    boundary->addPoint(x+dim,y-dim);
    boundary->addPoint(x+dim,y);
    boundary->closeRings();
    //the "Directly" variants assume ownership of the objects created above
    poly->addRingDirectly(boundary);
    feature->SetGeometryDirectly( poly );
}

auto get_world_index(int world_x, int world_y, int8_t dir) {
    switch (dir) {
        case 1: world_x--   ; world_y++; break;
        case 2:             ; world_y++; break;
        case 3: world_x++   ; world_y++; break;
        case 4: world_x--   ;          ; break;
        // case 5 induces no change
        case 6: world_x++   ;          ; break;
        case 7: world_x--   ; world_y--; break;
        case 8:             ; world_y--; break;
        case 9: world_x++   ; world_y--; break;
    }
    world_x = std::min(std::max(0,world_x),world->world_data->world_width - 1);
    world_y = std::min(std::max(0,world_y),world->world_data->world_height - 1);
    return std::pair(world_x,world_y);
}

auto create_field(OGRLayer *layer, std::string name, OGRFieldType type, int width = 0, OGRFieldSubType subtype = OFSTNone) {
    OGRFieldDefn field( name.c_str() , type );
    if (subtype != OFSTNone) {
        field.SetSubType(subtype);
    }
    if (width != 0) {
        field.SetWidth(width);
    }
    // this should create a copy internally
    if( layer->CreateField( &field ) != OGRERR_NONE ){
        throw CR_FAILURE;
    }
}

// PROJ.4 description of EPSG:3857 (https://epsg.io/3857)
static const char* EPSG_3857 = "+proj=merc +a=6378137 +b=6378137 +lat_ts=0 +lon_0=0 +x_0=0 +y_0=0 +k=1 +units=m +nadgrids=@null +wktext +no_defs +type=crs";



const char* describe_surroundings(int savagery, int evilness) {
    constexpr std::array<const char*,9>surroundings{
        "Serene",   "Mirthful",     "Joyous Wilds",
        "Calm",     "Wilderness",   "Untamed Wilds",
        "Sinister", "Haunted",      "Terrifying"
    };
    auto savagery_index = savagery < 33 ? 0 : (savagery > 65 ? 2 : 1);
    auto evilness_index = evilness < 33 ? 0 : (evilness > 65 ? 2 : 1);
    return surroundings[3 * evilness_index + savagery_index];
}

static command_result do_command(color_ostream &out, vector<string> &parameters) {
    // we don't want DF to delete region details while we are iterating over them...
    CoreSuspender suspend;

    if (!Core::getInstance().isWorldLoaded()){
        out.printerr("This command requires a world to be loaded\n");
        return CR_WRONG_USAGE;
    }

    out.print("%lu out of %d region map tiles loaded\n",
        world->world_data->midmap_data.region_details.size(),
        world->world_data->world_width * world->world_data->world_height
    );
    out.print("exporting map... ");
    out.flush();
    const auto start{std::chrono::steady_clock::now()};

    // set up coordinate system
    OGRSpatialReference CRS;
    if (CRS.importFromProj4(EPSG_3857) != OGRERR_NONE) {
        out.printerr("could not set up coordinate system");
        return CR_FAILURE;
    }

    // set up output driver
    GDALAllRegister();
    const char *driver_name = "Parquet";
    const char *extension = "parquet";
    // const char *driver_name = "GPKG";
    // const char *extension = "gpkg";
    auto driver = GetGDALDriverManager()->GetDriverByName(driver_name);
    CHECK_NULL_POINTER(driver);

    // create a data set and associate it to a file
    std::string map("map.");
    map.append(extension);
    auto dataset = driver->Create( map.c_str(), 0, 0, 0, GDT_Unknown, NULL );

    // create a layer for the biome data
    auto layer = dataset->CreateLayer( "world_biomes", &CRS, wkbPolygon, NULL );

    try {
        create_field(layer, "region_id", OFTInteger);
        create_field(layer, "region_name_en", OFTString, 100);
        create_field(layer, "region_name_df", OFTString, 100);
        create_field(layer, "landmass_id", OFTInteger);
        create_field(layer, "landmass_name_en", OFTString, 100);
        create_field(layer, "landmass_name_df", OFTString, 100);

        create_field(layer, "biome_type", OFTString, 32);
        create_field(layer, "surroundings", OFTString, 16);
        create_field(layer, "elevation", OFTInteger);

        create_field(layer, "evilness", OFTInteger);
        create_field(layer, "savagery", OFTInteger);
        create_field(layer, "volcanism", OFTInteger);
        create_field(layer, "drainage", OFTInteger);
        create_field(layer, "temperature", OFTInteger);
        create_field(layer, "vegetation", OFTInteger);
        create_field(layer, "rainfall", OFTInteger);
        create_field(layer, "snowfall", OFTInteger);
        create_field(layer, "salinity", OFTInteger);

        create_field(layer, "reanimating", OFTInteger, OFSTBoolean);
        create_field(layer, "has_bogeymen", OFTInteger, OFSTBoolean);

    }catch (const DFHack::command_result& r) {
        out.printerr("could not create fields for output layer");
        return r;
    }

    #define REGION 1
    #ifndef REGION
    for (int x = 0; x < world->world_data->world_width; ++x) {
        for (int y = 0; y < world->world_data->world_height; ++y) {
            // auto& entry = world->world_data->region_map[x][y];

            auto feature = OGRFeature::CreateFeature( layer->GetLayerDefn() );
            setGeometry(feature, x, y, 1.0);

            auto biome = ENUM_KEY_STR(biome_type, Maps::getBiomeType(x,y));
            feature->SetField( "biome_type", biome.c_str() );

            // updates the feature with the id it receives in the layer
            if( layer->CreateFeature( feature ) != OGRERR_NONE )
                return CR_FAILURE;

            OGRFeature::DestroyFeature( feature );
        }
    }Maps::getBiomeType(world_x + x_offset,world_y + y_offset));
    #else
    int wdim = 768; // dimension of a world tile
    int rdim = 48;  // dimension of a region tile
    for (auto const region_details : world->world_data->midmap_data.region_details) {
        auto world_x = region_details->pos.x;
        auto world_y = region_details->pos.y;
        for (int region_x = 0; region_x < 16; ++region_x) {
            for (int region_y = 0; region_y < 16; ++region_y) {

                auto feature = OGRFeature::CreateFeature( layer->GetLayerDefn() );
                setGeometry(
                    feature,
                    (double)(world_x * wdim + region_x * rdim),
                    (double)(world_y * wdim + region_y * rdim),
                    rdim
                );

                // get information from the region details
                auto [biome_x,biome_y] = get_world_index(world_x, world_y, region_details->biome[region_x][region_y]);
                feature->SetField( "biome_type", ENUM_KEY_STR(biome_type,Maps::getBiomeType(biome_x,biome_y)).c_str() );
                feature->SetField( "elevation", region_details->elevation[region_x][region_y]);

                // gets supplementary information from the world tile
                auto& world_entry = world->world_data->region_map[biome_x][biome_y];
                #define SET_FIELD(name) feature->SetField( #name, world_entry.name)
                SET_FIELD(region_id);
                SET_FIELD(landmass_id);
                SET_FIELD(evilness);
                SET_FIELD(savagery);
                SET_FIELD(volcanism);
                SET_FIELD(drainage);
                SET_FIELD(temperature);
                SET_FIELD(vegetation);
                SET_FIELD(rainfall);
                SET_FIELD(snowfall);
                SET_FIELD(salinity);
                #undef SET_FIELD

                feature->SetField( "surroundings", describe_surroundings(world_entry.savagery, world_entry.evilness));

                auto region = df::world_region::find(world_entry.region_id);
                if (region) {
                    auto region_name_en = DF2UTF(Translation::translateName(&region->name, true));
                    feature->SetField( "region_name_en", region_name_en.c_str());
                    auto region_name_df = DF2UTF(Translation::translateName(&region->name, false));
                    feature->SetField( "region_name_df", region_name_df.c_str());
                    feature->SetField("reanimating", region->reanimating);
                    feature->SetField("has_bogeymen", region->has_bogeymen);
                }
                auto landmass = df::world_landmass::find(world_entry.landmass_id);
                if (landmass) {
                    auto landmass_name_en = DF2UTF(Translation::translateName(&landmass->name, true));
                    feature->SetField( "landmass_name_en", landmass_name_en.c_str());
                    auto landmass_name_df = DF2UTF(Translation::translateName(&landmass->name, false));
                    feature->SetField( "landmass_name_df", landmass_name_df.c_str());
                }

                // this updates the feature with the id it receives in the layer
                if( layer->CreateFeature( feature ) != OGRERR_NONE )
                    return CR_FAILURE;
                // but we don't care and destroy the feature
                OGRFeature::DestroyFeature( feature );
            }
        }
    }
    #endif

    GDALClose( dataset );
    const auto finish{std::chrono::steady_clock::now()};
    const std::chrono::duration<double> elapsed_seconds{finish - start};
    out.print("done in %f ms !\n", elapsed_seconds.count());
    return CR_OK;
}
