"""Runtime Nanite spike, the border test after N3: tessellated copies of M_VoxelMaster_NaniteTess40 whose displacement
is a blend of per-material heights by the soft-border weights (VP2-O4), the data both copies of a duplicated vertex
share. N3's displacement went through FarFieldMaterialPick, which picks only in a pixel shader: in the displacement
pass it returns the quad's own material, which differs between the copies.

M_VoxelMaster_NaniteTessBlendConst40: each material a constant height (a hash of its ID): the mesh data alone.
M_VoxelMaster_NaniteTessBlend40: each material's albedo luminance, triplanar at a fixed mip, blended the same way.
Writes Saved/nanite_tess_blend.txt."""
import unreal

SRC = "/VoxelWorlds/Materials/M_VoxelMaster_NaniteTess40"
DIR = "/VoxelWorlds/Materials"
MEL = unreal.MaterialEditingLibrary
TRIPLANAR = "MF_TriplanarSampleAlbedoRoughness"
PICK = "FarFieldMaterialPick"

CODE = r"""
// The soft-border weights (VP2-O4), as FarFieldMaterialPick reads them, but blended instead of picked: alpha below
// 255 marks a blend, R the second material and 254 - A its weight, G the third and B its weight (0-63)
float W2 = 0.0;
float W3 = 0.0;
if (VertexAlpha < 254.5 / 255.0)
{
    W2 = saturate((254.0 / 255.0 - VertexAlpha) * (255.0 / 254.0));
    W3 = saturate(VertexB * (255.0 / 63.0));
}
float W1 = saturate(1.0 - W2 - W3);
float Ids[3] = { floor(MaterialID + 0.5), floor(SecondaryR * 255.0 + 0.5), floor(VertexG * 255.0 + 0.5) };
float Ws[3] = { W1, W2, W3 };
float3 TW = pow(abs(WorldNormal), Sharpness);
TW /= max(TW.x + TW.y + TW.z, 0.0001);
float2 UV_X = WorldPosition.zy * Scale;
float2 UV_Y = WorldPosition.xz * Scale;
float2 UV_Z = WorldPosition.xy * Scale;
float H = 0.0;
[unroll] for (int i = 0; i < 3; ++i)
{
    if (Ws[i] > 0.0005)
    {
        float Hi;
        if (Mode < 0.5)
        {
            Hi = 0.15 + 0.7 * frac(Ids[i] * 0.6180339);
        }
        else
        {
            int Id = (int)Ids[i];
            float Slice = floor(MaterialLUT.Load(int3(clamp(Id, 0, 255), 3, 0)).r * 255.0 + 0.5);
            float3 A = AlbedoArray.SampleLevel(AlbedoArraySampler, float3(UV_X, Slice), Mip).rgb * TW.x
                     + AlbedoArray.SampleLevel(AlbedoArraySampler, float3(UV_Y, Slice), Mip).rgb * TW.y
                     + AlbedoArray.SampleLevel(AlbedoArraySampler, float3(UV_Z, Slice), Mip).rgb * TW.z;
            Hi = saturate(dot(A, float3(0.299, 0.587, 0.114)));
        }
        H += Ws[i] * Hi;
    }
}
return H;
"""

out = []
tools = unreal.AssetToolsHelpers.get_asset_tools()


def find(exprs, pred):
    hits = [e for e in exprs if pred(e)]
    return hits[0] if hits else None


def sources_of(material, node):
    names = [str(n) for n in MEL.get_material_expression_input_names(node)]
    srcs = MEL.get_inputs_for_material_expression(material, node)
    result = {}
    for name, src in zip(names, srcs):
        if src is None:
            continue
        try:
            o = MEL.get_input_node_output_name_for_material_expression(node, src)
            if isinstance(o, tuple):
                o = o[-1] if o[0] else ""
        except Exception:  # noqa: BLE001
            o = ""
        result[name] = (src, o or "")
    return result


for name, mode in (("M_VoxelMaster_NaniteTessBlendConst40", 0.0), ("M_VoxelMaster_NaniteTessBlend40", 1.0)):
    path = DIR + "/" + name
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)
    M = tools.duplicate_asset(name, DIR, unreal.EditorAssetLibrary.load_asset(SRC))
    exprs = MEL.get_material_expressions(M)
    make = find(exprs, lambda e: isinstance(e, unreal.MaterialExpressionMakeMaterialAttributes))
    tri = find(exprs, lambda e: isinstance(e, unreal.MaterialExpressionMaterialFunctionCall)
               and e.get_editor_property("material_function") is not None
               and e.get_editor_property("material_function").get_name() == TRIPLANAR)
    pick = find(exprs, lambda e: isinstance(e, unreal.MaterialExpressionCustom)
                and PICK in str(e.get_editor_property("description")))
    out.append("%s: make=%s tri=%s pick=%s" % (name, make and make.get_name(), tri and tri.get_name(),
                                                 pick and pick.get_name()))
    if not (make and tri and pick):
        continue
    tri_src = sources_of(M, tri)
    pick_src = sources_of(M, pick)
    out.append("  tri inputs: %s" % {k: (v[0].get_name(), v[1]) for k, v in tri_src.items()})
    out.append("  pick inputs: %s" % {k: (v[0].get_name(), v[1]) for k, v in pick_src.items()})
    node = MEL.create_material_expression(M, unreal.MaterialExpressionCustom, -900, 1100)
    node.set_editor_property("description", "BlendedHeight")
    node.set_editor_property("code", CODE)
    node.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    names = ["MaterialID", "SecondaryR", "VertexAlpha", "VertexG", "VertexB", "WorldPosition", "WorldNormal",
             "Scale", "Sharpness", "AlbedoArray", "MaterialLUT", "Mode", "Mip"]
    inputs = []
    for n in names:
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", n)
        inputs.append(ci)
    node.set_editor_property("inputs", inputs)
    ok = {}
    # The output name lookup is per source node, so the vertex colour's four links all read as R: name them here
    for n, o in (("MaterialID", None), ("SecondaryR", "R"), ("VertexAlpha", "A"), ("VertexG", "G"), ("VertexB", "B")):
        src, found = pick_src[n]
        ok[n] = MEL.connect_material_expressions(src, found if o is None else o, node, n)
    for n in ("WorldPosition", "WorldNormal", "Scale", "Sharpness", "AlbedoArray", "MaterialLUT"):
        if n in tri_src:
            src, o = tri_src[n]
            ok[n] = MEL.connect_material_expressions(src, o, node, n)
        else:
            ok[n] = "MISSING"
    for n, v in (("Mode", mode), ("Mip", 2.0)):
        c = MEL.create_material_expression(M, unreal.MaterialExpressionConstant, -1100, 1300 if n == "Mode" else 1360)
        c.set_editor_property("r", v)
        ok[n] = MEL.connect_material_expressions(c, "", node, n)
    ok["Displacement"] = MEL.connect_material_expressions(node, "", make, "Displacement")
    out.append("  links: %s" % ok)
    MEL.recompile_material(M)
    unreal.EditorAssetLibrary.save_loaded_asset(M)
    out.append("  tess=%s scaling=%s" % (M.get_editor_property("enable_tessellation"),
                                        M.get_editor_property("displacement_scaling")))
with open(unreal.Paths.project_saved_dir() + "nanite_tess_blend.txt", "w", encoding="utf-8") as f:
    f.write("\n".join(out))
