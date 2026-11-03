"""Echoes Room Tools: build Metroid Prime 2: Echoes rooms in Blender for the native port.

Model a room, give its materials Echoes looks (built-in templates, or materials read off your own disc),
mark what collides, and Build: the add-on cooks the room (MREA) and its new textures into the port's mods
folder. Import a retail room as a reference to model against. Read docs in README.md beside this file.

The modules other than ui.py do not need Blender, so they can be tested and scripted on their own.
"""

bl_info = {
    "name": "Echoes Room Tools",
    "author": "Prime World Editor contributors",
    "version": (0, 1, 0),
    "blender": (4, 2, 0),
    "location": "3D Viewport > Sidebar > Echoes; Material Properties > Echoes Material",
    "description": "Build Metroid Prime 2: Echoes rooms (geometry, materials, collision) for the native port",
    "category": "Import-Export",
}


def register():
    from . import ui
    ui.register()


def unregister():
    from . import ui
    ui.unregister()
