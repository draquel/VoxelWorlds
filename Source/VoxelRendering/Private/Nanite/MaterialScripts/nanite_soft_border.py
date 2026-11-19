"""Runtime Nanite spike, the border test: M_VoxelMaster_Nanite's FarFieldMaterialPick picks in Nanite's shading pass
too. Its code was guarded by PIXELSHADER alone, which Nanite's compute shading does not define, so with the terrain on
Nanite every soft material border (VP2-O4) drew hard: each triangle its quad's own material. The guard now reads
PIXELSHADER || IS_NANITE_SHADING_PASS; Nanite's raster pass (where the displacement is evaluated) still takes the
quad's material, as before. Deletes the test copy M_VoxelMaster_NaniteSoft. Writes Saved/nanite_soft_border.txt.
Run nanite_tess_material.py and nanite_tess_blend.py after it: their materials are copies of this one."""
import unreal

PATH = "/VoxelWorlds/Materials/M_VoxelMaster_Nanite"
TEST = "/VoxelWorlds/Materials/M_VoxelMaster_NaniteSoft"
GUARD = "#if PIXELSHADER || IS_NANITE_SHADING_PASS"
MEL = unreal.MaterialEditingLibrary
out = []
if unreal.EditorAssetLibrary.does_asset_exist(TEST):
    out.append("test copy deleted %s" % unreal.EditorAssetLibrary.delete_asset(TEST))
M = unreal.EditorAssetLibrary.load_asset(PATH)
for e in MEL.get_material_expressions(M):
    if isinstance(e, unreal.MaterialExpressionCustom) and "FarFieldMaterialPick" in str(e.get_editor_property("description")):
        code = str(e.get_editor_property("code"))
        if GUARD in code:
            out.append("pick %s already guarded" % e.get_name())
            continue
        lines = code.splitlines(True)
        for i, line in enumerate(lines):
            if line.strip() == "#if PIXELSHADER":
                lines[i] = line.replace("#if PIXELSHADER", GUARD)
                break
        new = "".join(lines)
        out.append("pick %s changed=%s" % (e.get_name(), new != code))
        e.set_editor_property("code", new)
MEL.recompile_material(M)
unreal.EditorAssetLibrary.save_loaded_asset(M)
with open(unreal.Paths.project_saved_dir() + "nanite_soft_border.txt", "w", encoding="utf-8") as f:
    f.write("\n".join(out))
