#ifndef RENDERER_OBJ_H
#define RENDERER_OBJ_H

#include "renderer/mesh.h"

#include <stddef.h>

/*
 * Wavefront OBJ → wp_mesh_cpu. Positions required. Normals optional
 * (face normal if missing). Optional `v x y z r g b`. Triangulate n-gons
 * with a fan. CCW as written — do not reverse. See docs/MESH.md.
 *
 * Bad content returns a negative errno. The process does not exit.
 */

#define WP_OBJ_MAX_FILE (32u << 20)

[[nodiscard]] int wp_obj_parse(const char *text, size_t len, struct wp_mesh_cpu *out);
[[nodiscard]] int wp_obj_load(const char *path, struct wp_mesh_cpu *out);

#endif /* RENDERER_OBJ_H */
