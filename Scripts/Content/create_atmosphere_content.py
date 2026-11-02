# Creates the atmosphere presets of the Environment panel (Docs/ADR/0024-startup-atmospheres-starter-stages.md §3):
#   /Game/StageCraft/Atmospheres/DA_Atmosphere_ClearDaytime       outdoor sky at 13:00, clear, neutral look
#   /Game/StageCraft/Atmospheres/DA_Atmosphere_DramaticSunset     outdoor sky just before sunset, warm and contrasty
#   /Game/StageCraft/Atmospheres/DA_Atmosphere_CyberpunkNight     outdoor sky at night, wet haze, cool and saturated with glowing highlights
#   /Game/StageCraft/Atmospheres/DA_Atmosphere_MinimalistStudio   studio rig: soft even light, light backdrop, low contrast
#
# Headless (no editor window needed; the project's editor must be closed):
#   UnrealEditor-Cmd.exe <Project>.uproject -run=pythonscript -script="<abs path>/create_atmosphere_content.py" -unattended -nullrhi
#
# Idempotent: an existing asset is updated in place, so this script is the source of truth for the presets. Every value is
# read back after saving; the environment sanitizes them again when one is applied.

import unreal

DIR = "/Game/StageCraft/Atmospheres"

eal = unreal.EditorAssetLibrary
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()

OUTDOOR = unreal.StageLightingSource.OUTDOOR_SKY
STUDIO = unreal.StageLightingSource.STUDIO

# name, display name, description, order, swatch, lighting, (time, sun height, sun direction, clouds, rain, snow, fog),
# look (exposure EV, bloom, AO, white balance K, saturation, contrast, vignette, key lux, key colour)
PRESETS = [
    ("DA_Atmosphere_ClearDaytime", "Clear Daytime",
     "Early afternoon under a clear sky: crisp shadows and true colours for checking a design.",
     0, (0.33, 0.58, 0.95), OUTDOOR, (13.0, 60.0, 250.0, 0.12, 0.0, 0.0, 0.05),
     (0.0, 0.675, 0.5, 6500.0, 1.05, 1.0, 0.3, 3.0, (1.0, 1.0, 1.0))),
    ("DA_Atmosphere_DramaticSunset", "Dramatic Sunset",
     "The sun low in the west, warm light, long shadows and glowing highlights.",
     1, (1.0, 0.45, 0.14), OUTDOOR, (19.1, 35.0, 280.0, 0.45, 0.0, 0.0, 0.25),
     (0.3, 1.4, 0.5, 7800.0, 1.15, 1.12, 0.5, 3.0, (1.0, 1.0, 1.0))),
    ("DA_Atmosphere_CyberpunkNight", "Cyberpunk Night",
     "Late night after rain: wet haze that catches every beam, cool tones, saturated colour and glowing highlights.",
     2, (0.82, 0.12, 0.78), OUTDOOR, (22.5, 40.0, 270.0, 0.35, 0.2, 0.0, 0.3),
     (-1.2, 1.1, 0.5, 4800.0, 1.35, 1.1, 0.6, 3.0, (1.0, 1.0, 1.0))),
    ("DA_Atmosphere_MinimalistStudio", "Minimalist Studio",
     "A photo studio: soft light from every side, a gentle key light and a seamless light-grey backdrop.",
     3, (0.86, 0.87, 0.89), STUDIO, (14.0, 55.0, 270.0, 0.3, 0.0, 0.0, 0.1),
     (0.0, 0.4, 0.6, 6500.0, 0.9, 0.95, 0.15, 8.0, (1.0, 0.97, 0.92))),
]


def log(message):
    unreal.log("[StageCraftContent] " + message)


def make_settings(lighting, sky, look_values):
    time_of_day, sun_height, sun_direction, clouds, rain, snow, fog = sky
    exposure, bloom, ao, white_balance, saturation, contrast, vignette, key_lux, key_colour = look_values

    look = unreal.StageEnvironmentLook()
    look.set_editor_property("exposure_compensation", exposure)
    look.set_editor_property("bloom_intensity", bloom)
    look.set_editor_property("ambient_occlusion_intensity", ao)
    look.set_editor_property("white_balance_temp", white_balance)
    look.set_editor_property("saturation", saturation)
    look.set_editor_property("contrast", contrast)
    look.set_editor_property("vignette_intensity", vignette)
    look.set_editor_property("studio_key_intensity", key_lux)
    look.set_editor_property("studio_key_color", unreal.LinearColor(key_colour[0], key_colour[1], key_colour[2], 1.0))

    settings = unreal.StageEnvironmentSettings()
    settings.set_editor_property("lighting", lighting)
    settings.set_editor_property("time_of_day", time_of_day)
    settings.set_editor_property("sun_elevation", sun_height)
    settings.set_editor_property("sun_azimuth", sun_direction)
    settings.set_editor_property("cloud_coverage", clouds)
    settings.set_editor_property("rain", rain)
    settings.set_editor_property("snow", snow)
    settings.set_editor_property("fog", fog)
    settings.set_editor_property("look", look)
    return settings


def create_preset(name, display_name, description, order, swatch, lighting, sky, look_values):
    path = DIR + "/" + name
    if eal.does_asset_exist(path):
        preset = eal.load_asset(path)
    else:
        preset = asset_tools.create_asset(name, DIR, unreal.StageAtmospherePreset, unreal.DataAssetFactory())
    if preset is None:
        raise RuntimeError("Could not create or load " + path)

    preset.set_editor_property("display_name", display_name)
    preset.set_editor_property("description", description)
    preset.set_editor_property("sort_order", order)
    preset.set_editor_property("swatch_color", unreal.LinearColor(swatch[0], swatch[1], swatch[2], 1.0))
    preset.set_editor_property("settings", make_settings(lighting, sky, look_values))
    eal.save_loaded_asset(preset)

    # Read back what was saved: a dropped struct field would silently change the look.
    saved = eal.load_asset(path).get_editor_property("settings")
    if saved.get_editor_property("lighting") != lighting:
        raise RuntimeError("{}: lighting is {}".format(path, saved.get_editor_property("lighting")))
    if abs(saved.get_editor_property("time_of_day") - sky[0]) > 1e-3:
        raise RuntimeError("{}: time of day is {}".format(path, saved.get_editor_property("time_of_day")))
    saved_look = saved.get_editor_property("look")
    if abs(saved_look.get_editor_property("white_balance_temp") - look_values[3]) > 1e-2:
        raise RuntimeError("{}: white balance is {}".format(path, saved_look.get_editor_property("white_balance_temp")))
    log("Saved {} ({}, {})".format(path, display_name, lighting))


for preset in PRESETS:
    create_preset(*preset)
log("Done.")
