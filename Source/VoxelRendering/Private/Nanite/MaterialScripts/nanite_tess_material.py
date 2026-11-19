"""Runtime Nanite spike N3: tessellated copies of M_VoxelMaster_Nanite (M_VoxelMaster_NaniteTess10 and _Tess40).
Python cannot reach the Displacement pin (MP_Displacement is hidden from the enum), so each copy is switched to
material attributes: every connected output goes through one MakeMaterialAttributes node, and its Displacement pin
takes the albedo's luminance (the smooth / cubic albedo before the debug colour and the weather). Tessellation on,
the displacement scaling's magnitude 10 or 40 cm, centre 0.3. Writes Saved/nanite_tess_material.txt."""
import unreal

SRC = "/VoxelWorlds/Materials/M_VoxelMaster_Nanite"
DIR = "/VoxelWorlds/Materials"
CENTER = 0.3
mel = unreal.MaterialEditingLibrary
MP = unreal.MaterialProperty
PINS = [(MP.MP_BASE_COLOR, "BaseColor"), (MP.MP_METALLIC, "Metallic"), (MP.MP_SPECULAR, "Specular"),
        (MP.MP_ROUGHNESS, "Roughness"), (MP.MP_ANISOTROPY, "Anisotropy"), (MP.MP_EMISSIVE_COLOR, "EmissiveColor"),
        (MP.MP_OPACITY, "Opacity"), (MP.MP_OPACITY_MASK, "OpacityMask"), (MP.MP_NORMAL, "Normal"),
        (MP.MP_TANGENT, "Tangent"), (MP.MP_WORLD_POSITION_OFFSET, "WorldPositionOffset"),
        (MP.MP_SUBSURFACE_COLOR, "SubsurfaceColor"), (MP.MP_AMBIENT_OCCLUSION, "AmbientOcclusion"),
        (MP.MP_REFRACTION, "Refraction")]
out = []
tools = unreal.AssetToolsHelpers.get_asset_tools()
for name, magnitude in (("M_VoxelMaster_NaniteTess10", 10.0), ("M_VoxelMaster_NaniteTess40", 40.0)):
    path = DIR + "/" + name
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)
    M = tools.duplicate_asset(name, DIR, unreal.EditorAssetLibrary.load_asset(SRC))
    make = mel.create_material_expression(M, unreal.MaterialExpressionMakeMaterialAttributes, -300, 600)
    for prop, pin in PINS:
        node = mel.get_material_property_input_node(M, prop)
        if not node:
            continue
        try:
            output = mel.get_material_property_input_node_output_name(M, prop)
        except Exception:  # noqa: BLE001
            output = ""
        ok = mel.connect_material_expressions(node, output, make, pin)
        out.append("%s: %s <- %s[%s] %s" % (name, pin, node.get_name(), output, ok))
    # The albedo: BaseColor <- ComponentMask <- WeatherSurface custom <- debug lerp <- smooth/cubic lerp
    mask = mel.get_material_property_input_node(M, MP.MP_BASE_COLOR)
    weather = mel.get_inputs_for_material_expression(M, mask)[0]
    debug = mel.get_inputs_for_material_expression(M, weather)[0]
    albedo = mel.get_inputs_for_material_expression(M, debug)[0]
    out.append("%s: albedo = %s (%s)" % (name, albedo.get_name(), albedo.get_class().get_name()))
    weights = mel.create_material_expression(M, unreal.MaterialExpressionConstant3Vector, -700, 900)
    weights.set_editor_property("constant", unreal.LinearColor(0.299, 0.587, 0.114, 0.0))
    dot = mel.create_material_expression(M, unreal.MaterialExpressionDotProduct, -500, 900)
    mel.connect_material_expressions(albedo, "", dot, "A")
    mel.connect_material_expressions(weights, "", dot, "B")
    sat = mel.create_material_expression(M, unreal.MaterialExpressionSaturate, -400, 900)
    mel.connect_material_expressions(dot, "", sat, "")
    out.append("%s: displacement pin %s" % (name, mel.connect_material_expressions(sat, "", make, "Displacement")))
    M.set_editor_property("use_material_attributes", True)
    out.append("%s: attributes %s" % (name, mel.connect_material_property(make, "", MP.MP_MATERIAL_ATTRIBUTES)))
    M.set_editor_property("enable_tessellation", True)
    M.set_editor_property("displacement_scaling", unreal.DisplacementScaling(magnitude=magnitude, center=CENTER))
    mel.recompile_material(M)
    unreal.EditorAssetLibrary.save_loaded_asset(M)
    out.append("%s: tess=%s scaling=%s attrs=%s" % (name, M.get_editor_property("enable_tessellation"),
                                                    M.get_editor_property("displacement_scaling"),
                                                    M.get_editor_property("use_material_attributes")))
with open(unreal.Paths.project_saved_dir() + "nanite_tess_material.txt", "w", encoding="utf-8") as f:
    f.write("\n".join(out))
