"""Blender side of Echoes Room Tools: preferences, Echoes settings on materials and objects, the
sidebar panel, and the operators (import a room as a reference, new template material, build the room
into the port's mods folder). Everything that does not need Blender is in the other modules.
"""
import os

import bpy
import numpy as np
from bpy.props import BoolProperty, CollectionProperty, EnumProperty, IntProperty, PointerProperty, StringProperty

from . import build as bld
from . import collision as col
from . import disc as disc_mod
from . import materials as mats
from . import room_reader
from . import txtr

ADDON = __package__

# ---------------------------------------------------------------------------- disc cache
_disc = {"path": None, "disc": None, "error": None}
_enum_cache = {"worlds": [], "areas": []}   # Blender needs enum item strings kept alive


def prefs(context=None):
    context = context or bpy.context
    return context.preferences.addons[ADDON].preferences


def get_disc(context=None):
    path = bpy.path.abspath(prefs(context).disc_path) if prefs(context).disc_path else ""
    if _disc["path"] != path:
        if _disc["disc"] is not None:
            _disc["disc"].close()
        _disc.update(path=path, disc=None, error=None)
        try:
            _disc["disc"] = disc_mod.Disc(path)
        except Exception as e:   # shown in the panel
            _disc["error"] = str(e)
    return _disc["disc"]


def _world(context):
    d = get_disc(context)
    if d is None:
        return None
    key = context.scene.echoes_room.world
    return next((w for w in d.worlds() if str(w["mlvl"]) == key), None)


def _area(context):
    w = _world(context)
    if w is None:
        return None, None
    key = context.scene.echoes_room.area
    return w, next((a for a in w["areas"] if str(a["index"]) == key), None)


def world_items(self, context):
    d = get_disc(context)
    items = [(str(w["mlvl"]), w["name"], f"MLVL {w['mlvl']:08X} in {w['pak']}") for w in d.worlds()] if d else []
    _enum_cache["worlds"] = items or [("NONE", "(no disc)", "Set the disc image in the add-on's preferences")]
    return _enum_cache["worlds"]


def area_items(self, context):
    w = _world(context)
    items = ([(str(a["index"]), f"{a['index']}: {a['name']}", f"{a['iname']}, MREA {a['mrea']:08X}") for a in w["areas"]]
             if w else [])
    _enum_cache["areas"] = items or [("NONE", "(no world)", "")]
    return _enum_cache["areas"]


# ---------------------------------------------------------------------------- properties
class EchoesPreferences(bpy.types.AddonPreferences):
    bl_idname = ADDON
    disc_path: StringProperty(name="Disc image", subtype="FILE_PATH",
                              description="Your Metroid Prime 2: Echoes disc image (.iso, .gcm or .ciso)")
    mods_path: StringProperty(name="Port mods folder", subtype="DIR_PATH",
                              description="The port's mods folder (on Windows, user\\mods beside metroid_prime2_port.exe)")

    def draw(self, context):
        self.layout.prop(self, "disc_path")
        self.layout.prop(self, "mods_path")


class EchoesTextureOverride(bpy.types.PropertyGroup):
    slot: IntProperty(name="Slot", min=0, description="The disc material's texture slot to replace")
    image: PointerProperty(name="Image", type=bpy.types.Image)


def _update_preview(self, context):
    mat = self.id_data
    if isinstance(mat, bpy.types.Material):
        build_preview_nodes(mat)


class EchoesMaterialProps(bpy.types.PropertyGroup):
    source: EnumProperty(name="Source", items=[("TEMPLATE", "Template", "A built-in Echoes material"),
                                               ("DISC", "From disc", "A material read off the disc")],
                         default="TEMPLATE", update=_update_preview)
    template: EnumProperty(name="Template", items=[(k, k.title(), v) for k, v in mats.TEMPLATES.items()],
                           default="LIT", update=_update_preview)
    diffuse: PointerProperty(name="Texture", type=bpy.types.Image, update=_update_preview,
                             description="The surface's texture (sizes multiples of 4; powers of two are safest)")
    lightmap: PointerProperty(name="Lightmap", type=bpy.types.Image, update=_update_preview,
                              description="Baked light for the Lightmapped template, mapped with the 'Lightmap' UV map")
    disc_data: StringProperty(name="Disc material", options={"HIDDEN"})
    disc_textures: StringProperty(name="Disc textures", options={"HIDDEN"})
    disc_origin: StringProperty(name="Origin")
    overrides: CollectionProperty(type=EchoesTextureOverride)
    collides: BoolProperty(name="Collides", default=True, description="Faces with this material are solid")
    surface: EnumProperty(name="Surface", items=[(k, k.replace("_", " ").title(), "") for k in col.SURFACES],
                          default="STONE", description="What the surface is made of (footsteps, effects)")
    shoot_through: BoolProperty(name="Shots pass")
    camera_through: BoolProperty(name="Camera passes")
    scan_through: BoolProperty(name="Scans pass")
    see_through: BoolProperty(name="See-through")
    ai_through: BoolProperty(name="Enemies pass")


ROLE_ITEMS = [("BOTH", "Drawn and solid", ""), ("VISUAL", "Drawn only", ""), ("COLLISION", "Collision only", "")]


class EchoesSceneProps(bpy.types.PropertyGroup):
    world: EnumProperty(name="World", items=world_items)
    area: EnumProperty(name="Room", items=area_items)
    mod_name: StringProperty(name="Mod", default="my_room", description="The mod folder the room is written to")
    clear_path: BoolProperty(name="Clear pathfinding", default=False,
                             description="Drop the room's old PATH (enemies that walk by path then stay put)")
    last_report: StringProperty(name="Last build")


# ---------------------------------------------------------------------------- images and preview nodes
def image_to_input(image: bpy.types.Image) -> bld.ImageInput:
    w, h = image.size
    if w == 0 or h == 0:
        raise bld.BuildError(f"image {image.name!r} has no pixels (is the file missing?)")
    px = np.empty(w * h * 4, dtype=np.float32)
    image.pixels.foreach_get(px)
    rgba = (np.clip(px.reshape(h, w, 4), 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)   # bottom row first
    return bld.ImageInput(image.name, rgba)


def texture_image(disc, tid: int) -> bpy.types.Image:
    name = f"TXTR_{tid:08X}"
    image = bpy.data.images.get(name)
    if image is None:
        rgba = txtr.decode(disc.get("TXTR", tid))[::-1]   # Blender stores the bottom row first
        h, w = rgba.shape[:2]
        image = bpy.data.images.new(name, w, h, alpha=True)
        image.pixels.foreach_set((rgba.astype(np.float32) / 255.0).ravel())
        image.pack()
    return image


def _disc_role_image(mat, role):
    props = mat.echoes
    ids = [int(t, 16) for t in props.disc_textures.split(",") if t]
    try:
        roles = mats.parse_material(bytes.fromhex(props.disc_data)).roles()
    except Exception:
        return None
    slot = roles.get(role)
    if slot is None or slot >= len(ids):
        return None
    for o in props.overrides:
        if o.slot == slot and o.image:
            return o.image
    return bpy.data.images.get(f"TXTR_{ids[slot]:08X}")


def build_preview_nodes(mat: bpy.types.Material):
    """An approximation of the Echoes look for Blender's viewport (the game draws its own TEV)."""
    props = mat.echoes
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    out.location = (600, 0)
    bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
    bsdf.location = (300, 0)
    nt.links.new(bsdf.outputs[0], out.inputs[0])
    if props.source == "TEMPLATE":
        diffuse, lightmap, kind = props.diffuse, props.lightmap if props.template == "LIGHTMAPPED" else None, props.template
    else:
        diffuse, lightmap, kind = _disc_role_image(mat, "diffuse"), _disc_role_image(mat, "lightmap"), "DISC"
    color = None
    if diffuse:
        tex = nt.nodes.new("ShaderNodeTexImage")
        tex.image, tex.location = diffuse, (-400, 100)
        uv = nt.nodes.new("ShaderNodeUVMap")
        uv.uv_map, uv.location = "UVMap", (-650, 100)
        nt.links.new(uv.outputs[0], tex.inputs[0])
        color = tex.outputs["Color"]
        nt.links.new(tex.outputs["Alpha"], bsdf.inputs["Alpha"])
    if diffuse and lightmap:
        lm = nt.nodes.new("ShaderNodeTexImage")
        lm.image, lm.location = lightmap, (-400, -200)
        uv2 = nt.nodes.new("ShaderNodeUVMap")
        uv2.uv_map, uv2.location = "Lightmap", (-650, -200)
        nt.links.new(uv2.outputs[0], lm.inputs[0])
        mix = nt.nodes.new("ShaderNodeMix")
        mix.data_type, mix.blend_type, mix.location = "RGBA", "MULTIPLY", (0, 0)
        mix.inputs["Factor"].default_value = 1.0
        nt.links.new(color, mix.inputs[6])
        nt.links.new(lm.outputs["Color"], mix.inputs[7])
        color = mix.outputs[2]
    if color is not None:
        if kind in ("UNLIT", "ADDITIVE"):
            nt.links.new(color, bsdf.inputs["Emission Color"])
            bsdf.inputs["Emission Strength"].default_value = 1.0
            bsdf.inputs["Base Color"].default_value = (0, 0, 0, 1)
        else:
            nt.links.new(color, bsdf.inputs["Base Color"])
    if hasattr(mat, "surface_render_method"):
        mat.surface_render_method = "BLENDED" if kind == "ADDITIVE" else "DITHERED"


# ---------------------------------------------------------------------------- gathering the scene
def _collision_input(props) -> bld.CollisionInput:
    flags = tuple(f for f, on in (("SHOOT_THROUGH", props.shoot_through), ("CAMERA_THROUGH", props.camera_through),
                                  ("SCAN_THROUGH", props.scan_through), ("SEE_THROUGH", props.see_through),
                                  ("AI_THROUGH", props.ai_through)) if on)
    return bld.CollisionInput(props.collides, props.surface, flags)


def material_input(mat: bpy.types.Material, images: dict) -> bld.MaterialInput:
    props = mat.echoes

    def img(image):
        if image.name not in images:
            images[image.name] = image_to_input(image)
        return images[image.name]
    if props.source == "TEMPLATE":
        given = {}
        if props.diffuse:
            given["diffuse"] = img(props.diffuse)
        if props.template == "LIGHTMAPPED" and props.lightmap:
            given["lightmap"] = img(props.lightmap)
        return bld.MaterialInput(mat.name, "TEMPLATE", props.template, given, collision=_collision_input(props))
    if not props.disc_data:
        raise bld.BuildError(f"material {mat.name!r} is 'From disc' but holds no disc material (import one from a room)")
    overrides = {o.slot: img(o.image) for o in props.overrides if o.image}
    return bld.MaterialInput(mat.name, "DISC", disc_material=bytes.fromhex(props.disc_data),
                             disc_texture_ids=[int(t, 16) for t in props.disc_textures.split(",") if t],
                             overrides=overrides, collision=_collision_input(props))


def mesh_input(obj, depsgraph, material_index: dict, materials: list, images: dict) -> bld.MeshInput:
    """The evaluated mesh (modifiers applied) in world space, triangulated, with its UV maps
    ('UVMap' or the active one, and 'Lightmap'), corner normals and colours."""
    evaluated = obj.evaluated_get(depsgraph)
    mesh = evaluated.to_mesh()
    try:
        mesh.calc_loop_triangles()
        world = obj.matrix_world
        normal_matrix = world.to_3x3().inverted_safe().transposed()
        flip = world.determinant() < 0   # a mirrored object turns its faces inside out
        uv_layer = mesh.uv_layers.get("UVMap") or mesh.uv_layers.active
        lightmap_layer = mesh.uv_layers.get("Lightmap")
        color_attr = mesh.color_attributes.active_color if hasattr(mesh, "color_attributes") else None
        corner_normals = mesh.corner_normals if hasattr(mesh, "corner_normals") else None
        positions = [tuple(world @ v.co) for v in mesh.vertices]
        tris, tri_mats = [], []
        for lt in mesh.loop_triangles:
            if lt.material_index >= len(obj.material_slots) or obj.material_slots[lt.material_index].material is None:
                raise bld.BuildError(f"object {obj.name!r} has faces with no material")
            mat = obj.material_slots[lt.material_index].material
            if mat.name not in material_index:
                material_index[mat.name] = len(materials)
                materials.append(material_input(mat, images))
            corners = []
            for loop, vert in zip(lt.loops, lt.vertices):
                n = corner_normals[loop].vector if corner_normals is not None else mesh.loops[loop].normal
                n = (normal_matrix @ n).normalized()
                uv = tuple(uv_layer.data[loop].uv) if uv_layer else (0.0, 0.0)
                lm = tuple(lightmap_layer.data[loop].uv) if lightmap_layer else uv
                color = 0xFFFFFFFF
                if color_attr is not None and color_attr.domain == "CORNER":
                    r, g, b, a = (max(0, min(255, int(c * 255 + 0.5))) for c in color_attr.data[loop].color)
                    color = r << 24 | g << 16 | b << 8 | a
                corners.append(bld.Corner(positions[vert], tuple(n), uv, lm, color))
            if flip:
                corners[1], corners[2] = corners[2], corners[1]
            tris.append(tuple(corners))
            tri_mats.append(material_index[mat.name])
            if mat.echoes.source == "TEMPLATE" and mat.echoes.template == "LIGHTMAPPED" and lightmap_layer is None:
                raise bld.BuildError(f"object {obj.name!r} uses Lightmapped material {mat.name!r} but has no UV map "
                                     "named 'Lightmap' (add one, then unwrap it with Lightmap Pack)")
        return bld.MeshInput(obj.name, tris, tri_mats, obj.echoes_role)
    finally:
        evaluated.to_mesh_clear()


# ---------------------------------------------------------------------------- operators
class ECHOES_OT_new_material(bpy.types.Operator):
    bl_idname = "echoes.new_material"
    bl_label = "New Echoes Material"
    bl_description = "Add a material from an Echoes template to the active object"
    bl_options = {"REGISTER", "UNDO"}
    template: EnumProperty(name="Template", items=[(k, k.title(), v) for k, v in mats.TEMPLATES.items()])

    def execute(self, context):
        mat = bpy.data.materials.new(f"Echoes {self.template.title()}")
        mat.echoes.source, mat.echoes.template = "TEMPLATE", self.template
        build_preview_nodes(mat)
        obj = context.active_object
        if obj is not None and obj.type == "MESH":
            obj.data.materials.append(mat)
            obj.active_material_index = len(obj.material_slots) - 1
        return {"FINISHED"}


class ECHOES_OT_add_override(bpy.types.Operator):
    bl_idname = "echoes.add_override"
    bl_label = "Replace a Texture"
    bl_description = "Replace one of this disc material's textures with an image (a new lightmap, say)"

    def execute(self, context):
        mat = context.material
        if mat is not None:
            o = mat.echoes.overrides.add()
            roles = {}
            try:
                roles = mats.parse_material(bytes.fromhex(mat.echoes.disc_data)).roles()
            except Exception:
                pass
            o.slot = roles.get("lightmap", 0)
        return {"FINISHED"}


class ECHOES_OT_import_room(bpy.types.Operator):
    bl_idname = "echoes.import_room"
    bl_label = "Import Room as Reference"
    bl_description = ("Import the selected room's world geometry and materials from the disc, as a reference to "
                      "model against (not built unless you include it)")
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        d = get_disc(context)
        world, area = _area(context)
        if d is None or area is None:
            self.report({"ERROR"}, _disc["error"] or "Pick a world and a room first")
            return {"CANCELLED"}
        try:
            geom = room_reader.read_room(d.get("MREA", area["mrea"]))
        except Exception as e:
            self.report({"ERROR"}, f"Cannot read {area['name']}: {e}")
            return {"CANCELLED"}
        coll = bpy.data.collections.new(f"Echoes: {area['name']} (reference)")
        context.scene.collection.children.link(coll)
        # Materials, with their textures decoded off the disc.
        blender_mats = []
        for i, data in enumerate(geom.material_set.materials):
            m = mats.parse_material(data)
            ids = [geom.material_set.texture_ids[t] for t in m.textures]
            mat = bpy.data.materials.new(f"{area['iname']} {i}")
            mat.echoes.source = "DISC"
            mat.echoes.disc_data = data.hex()
            mat.echoes.disc_textures = ",".join(f"{t:08X}" for t in ids)
            mat.echoes.disc_origin = f"{world['name']} / {area['name']} / material {i}"
            for tid in ids:
                try:
                    texture_image(d, tid)
                except Exception:
                    pass
            build_preview_nodes(mat)
            blender_mats.append((mat, m))
        for mi, model in enumerate(geom.models):
            faces, face_mats, uv0, uv1, normals = [], [], [], [], []
            for surf in model.surfaces:
                if surf.material >= len(blender_mats):
                    continue
                info = blender_mats[surf.material][1]
                lm_attr = info.lightmap_attr()
                packed = bool(info.flags & mats.LIGHTMAP_UV_ARRAY)
                diffuse_attr = next((a for a in info.tex_attrs() if a != lm_attr), None)
                for tri in surf.triangles:
                    idx = [c[0] for c in tri]
                    if len(set(idx)) < 3:
                        continue
                    faces.append(idx)
                    face_mats.append(surf.material)
                    for c in tri:
                        n = model.normals[c[1]] if c[1] is not None and c[1] < len(model.normals) else (0.0, 0.0, 1.0)
                        normals.append(n)

                        def uv_of(attr):
                            if attr is None or c[4 + attr] is None:
                                return (0.0, 0.0)
                            arr = model.lightmap_texcoords if (packed and attr == 0) else model.texcoords
                            u, v = arr[c[4 + attr]] if c[4 + attr] < len(arr) else (0.0, 0.0)
                            return (u, 1.0 - v)
                        uv0.append(uv_of(diffuse_attr))
                        uv1.append(uv_of(lm_attr))
            if not faces:
                continue
            mesh = bpy.data.meshes.new(f"{area['iname']} model {mi}")
            mesh.from_pydata([tuple(p) for p in model.positions], [], faces)
            for mat, _ in blender_mats:
                mesh.materials.append(mat)
            for poly, fm in zip(mesh.polygons, face_mats):
                poly.material_index = fm
            for name, data in (("UVMap", uv0), ("Lightmap", uv1)):
                layer = mesh.uv_layers.new(name=name)
                layer.data.foreach_set("uv", [x for uv in data for x in uv])
            try:
                mesh.normals_split_custom_set(normals)
            except Exception:
                pass
            mesh.validate()
            obj = bpy.data.objects.new(mesh.name, mesh)
            obj.echoes_reference = True
            obj.echoes_role = "VISUAL"
            coll.objects.link(obj)
        self.report({"INFO"}, f"Imported {len(geom.models)} models and {len(blender_mats)} materials from {area['name']}")
        return {"FINISHED"}


class ECHOES_OT_build_room(bpy.types.Operator):
    bl_idname = "echoes.build_room"
    bl_label = "Build Room into Mods Folder"
    bl_description = ("Cook every visible mesh (except references not included) into the selected room and write "
                      "it, with its new textures, into the port's mods folder")

    def execute(self, context):
        p = prefs(context)
        d = get_disc(context)
        world, area = _area(context)
        if d is None or area is None:
            self.report({"ERROR"}, _disc["error"] or "Pick a world and a room first")
            return {"CANCELLED"}
        mods = bpy.path.abspath(p.mods_path) if p.mods_path else ""
        if not mods or not os.path.isdir(mods):
            self.report({"ERROR"}, "Set the port's mods folder in the add-on's preferences")
            return {"CANCELLED"}
        settings = context.scene.echoes_room
        mod_name = settings.mod_name.strip()
        if not mod_name or mod_name.startswith(".") or any(ch in mod_name for ch in '/\\:*?"<>|'):
            self.report({"ERROR"}, "The mod name must be one folder name")
            return {"CANCELLED"}
        depsgraph = context.evaluated_depsgraph_get()
        material_index, materials, images, meshes = {}, [], {}, []
        try:
            for obj in context.scene.objects:
                if obj.type != "MESH" or not obj.visible_get() or (obj.echoes_reference and not obj.echoes_include):
                    continue   # hidden meshes and references (unless included) stay out of the room
                meshes.append(mesh_input(obj, depsgraph, material_index, materials, images))
            mlvl = d.get("MLVL", world["mlvl"])
            inp = bld.BuildInput(base_mrea=d.get("MREA", area["mrea"]), mrea_id=area["mrea"], pak=d.pak_of("MREA", area["mrea"]),
                                 meshes=meshes, materials=materials, disc_ids=set(d.ids), clear_path=settings.clear_path,
                                 area_bounds=bld.area_bounds_from_mlvl(mlvl, area["index"]))
            result = bld.build(inp)
        except (bld.BuildError, disc_mod.DiscError) as e:
            self.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        root = os.path.join(mods, mod_name)
        for rel, data in result.files.items():
            path = os.path.join(root, *rel.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
        r = result.report
        summary = (f"{area['name']}: {r['world_models']} models, {r['area_surfaces']} surfaces, "
                   f"{r['collision_triangles']} collision triangles, {len(r['new_textures'])} new textures -> {root}")
        settings.last_report = summary + "".join(f"\n{w}" for w in r["warnings"])
        for w in r["warnings"]:
            self.report({"WARNING"}, w)
        self.report({"INFO"}, summary)
        return {"FINISHED"}


class ECHOES_OT_reload_disc(bpy.types.Operator):
    bl_idname = "echoes.reload_disc"
    bl_label = "Reload Disc"
    bl_description = "Read the disc image again"

    def execute(self, context):
        if _disc["disc"] is not None:
            _disc["disc"].close()
        _disc.update(path=None, disc=None, error=None)
        get_disc(context)
        return {"FINISHED"}


# ---------------------------------------------------------------------------- panels
class VIEW3D_PT_echoes_room(bpy.types.Panel):
    bl_label = "Echoes Room"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Echoes"

    def draw(self, context):
        layout = self.layout
        settings = context.scene.echoes_room
        d = get_disc(context)
        if d is None:
            layout.label(text=_disc["error"] or "Set the disc image in the add-on's preferences", icon="ERROR")
            layout.operator("echoes.reload_disc", icon="FILE_REFRESH")
            return
        layout.prop(settings, "world")
        layout.prop(settings, "area")
        layout.operator("echoes.import_room", icon="IMPORT")
        layout.separator()
        layout.operator_menu_enum("echoes.new_material", "template", text="New Echoes Material", icon="MATERIAL")
        layout.separator()
        layout.prop(settings, "mod_name")
        layout.prop(settings, "clear_path")
        layout.operator("echoes.build_room", icon="EXPORT")
        if settings.last_report:
            box = layout.box()
            for line in settings.last_report.split("\n"):
                box.label(text=line)


class MATERIAL_PT_echoes(bpy.types.Panel):
    bl_label = "Echoes Material"
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "material"

    @classmethod
    def poll(cls, context):
        return context.material is not None

    def draw(self, context):
        layout = self.layout
        props = context.material.echoes
        layout.prop(props, "source", expand=True)
        if props.source == "TEMPLATE":
            layout.prop(props, "template")
            layout.label(text=mats.TEMPLATES[props.template])
            layout.template_ID(props, "diffuse", open="image.open")
            if props.template == "LIGHTMAPPED":
                layout.template_ID(props, "lightmap", open="image.open")
        else:
            layout.label(text=props.disc_origin or "(no disc material: import a room)")
            if props.disc_data:
                try:
                    roles = mats.parse_material(bytes.fromhex(props.disc_data)).roles()
                    ids = props.disc_textures.split(",")
                    for role, slot in sorted(roles.items(), key=lambda kv: kv[1]):
                        layout.label(text=f"slot {slot}: {role} ({ids[slot] if slot < len(ids) else '?'})")
                except Exception as e:
                    layout.label(text=f"unreadable: {e}", icon="ERROR")
                for o in props.overrides:
                    row = layout.row()
                    row.prop(o, "slot")
                    row.template_ID(o, "image", open="image.open")
                layout.operator("echoes.add_override", icon="ADD")
        box = layout.box()
        box.prop(props, "collides")
        if props.collides:
            box.prop(props, "surface")
            row = box.row()
            row.prop(props, "shoot_through")
            row.prop(props, "camera_through")
            row = box.row()
            row.prop(props, "scan_through")
            row.prop(props, "see_through")
            box.prop(props, "ai_through")


class OBJECT_PT_echoes(bpy.types.Panel):
    bl_label = "Echoes"
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "object"

    @classmethod
    def poll(cls, context):
        return context.object is not None and context.object.type == "MESH"

    def draw(self, context):
        obj = context.object
        self.layout.prop(obj, "echoes_role")
        if obj.echoes_reference:
            self.layout.prop(obj, "echoes_include")


CLASSES = (EchoesPreferences, EchoesTextureOverride, EchoesMaterialProps, EchoesSceneProps, ECHOES_OT_new_material,
           ECHOES_OT_add_override, ECHOES_OT_import_room, ECHOES_OT_build_room, ECHOES_OT_reload_disc,
           VIEW3D_PT_echoes_room, MATERIAL_PT_echoes, OBJECT_PT_echoes)


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Material.echoes = PointerProperty(type=EchoesMaterialProps)
    bpy.types.Scene.echoes_room = PointerProperty(type=EchoesSceneProps)
    bpy.types.Object.echoes_role = EnumProperty(name="In the room", items=ROLE_ITEMS, default="BOTH")
    bpy.types.Object.echoes_reference = BoolProperty(name="Reference", default=False, options={"HIDDEN"})
    bpy.types.Object.echoes_include = BoolProperty(name="Include in build", default=False,
                                                   description="Build this imported reference into the room too")


def unregister():
    del bpy.types.Object.echoes_include
    del bpy.types.Object.echoes_reference
    del bpy.types.Object.echoes_role
    del bpy.types.Scene.echoes_room
    del bpy.types.Material.echoes
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
