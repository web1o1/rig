#ifndef _RUT_ASSET_H_
#define _RUT_ASSET_H_

#include <stdbool.h>
#include <gio/gio.h>
#include <cogl-gst/cogl-gst.h>

#include "rut-types.h"
#include "rut-mesh.h"

/* XXX: The definition of an "asset" is getting a big confusing.
 * Initially it used to represent things created in third party
 * programs that you might want to import into Rig. It lets us
 * keep track of the original path, create a thumbnail and track
 * tags for use in the Rig editor.
 *
 * We have been creating components with RutAsset properties
 * though and when we load a UI or send it across to a slave then
 * we are doing redundant work to create models and thumbnails
 * which are only useful to an editor.
 *
 * XXX: Maybe we can introduce the idea of a "Blob" which can
 * track the same kind of data we currently use assets for and
 * perhaps rename RutAsset to RigAsset and clarify that it is
 * only for use in the editor. A Blob can have an optional
 * back-reference to an asset, but when serializing to slaves
 * for example the assets wouldn't be kept.
 */

extern RutType rut_asset_type;
typedef struct _RutAsset RutAsset;
#define RUT_ASSET(X) ((RutAsset *)X)

void _rut_asset_type_init (void);

CoglBool
rut_file_info_is_asset (GFileInfo *info, const char *name);

RutAsset *
rut_asset_new_builtin (RutContext *ctx,
                       const char *icon_path);

RutAsset *
rut_asset_new_texture (RutContext *ctx,
                       const char *path,
                       const GList *inferred_tags);

RutAsset *
rut_asset_new_normal_map (RutContext *ctx,
                          const char *path,
                          const GList *inferred_tags);

RutAsset *
rut_asset_new_alpha_mask (RutContext *ctx,
                          const char *path,
                          const GList *inferred_tags);

RutAsset *
rut_asset_new_ply_model (RutContext *ctx,
                         const char *path,
                         const GList *inferred_tags);

RutAsset *
rut_asset_new_from_data (RutContext *ctx,
                         const char *path,
                         RutAssetType type,
                         bool is_video,
                         const uint8_t *data,
                         size_t len);

RutAsset *
rut_asset_new_from_mesh (RutContext *ctx,
                         RutMesh *mesh);

RutAssetType
rut_asset_get_type (RutAsset *asset);

const char *
rut_asset_get_path (RutAsset *asset);

RutContext *
rut_asset_get_context (RutAsset *asset);

CoglTexture *
rut_asset_get_texture (RutAsset *asset);

RutMesh *
rut_asset_get_mesh (RutAsset *asset);

RutObject *
rut_asset_get_model (RutAsset *asset);

CoglBool
rut_asset_get_is_video (RutAsset *asset);

void
rut_asset_set_inferred_tags (RutAsset *asset,
                             const GList *inferred_tags);

const GList *
rut_asset_get_inferred_tags (RutAsset *asset);

CoglBool
rut_asset_has_tag (RutAsset *asset, const char *tag);

GList *
rut_infer_asset_tags (RutContext *ctx, GFileInfo *info, GFile *asset_file);

void
rut_asset_add_inferred_tag (RutAsset *asset, const char *tag);

bool
rut_asset_needs_thumbnail (RutAsset *asset);

typedef void (*RutThumbnailCallback) (RutAsset *asset, void *user_data);

RutClosure *
rut_asset_thumbnail (RutAsset *asset,
                     RutThumbnailCallback ready_callback,
                     void *user_data,
                     RutClosureDestroyCallback destroy_cb);

void *
rut_asset_get_data (RutAsset *asset);

size_t
rut_asset_get_data_len (RutAsset *asset);

#endif /* _RUT_ASSET_H_ */
