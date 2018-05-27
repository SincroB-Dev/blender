/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Contributor(s): Dalai Felinto
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/blenkernel/intern/layer.c
 *  \ingroup bke
 */

#include <string.h>

#include "BLI_array.h"
#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_string_utils.h"
#include "BLI_threads.h"
#include "BLT_translation.h"

#include "BKE_animsys.h"
#include "BKE_collection.h"
#include "BKE_freestyle.h"
#include "BKE_global.h"
#include "BKE_idprop.h"
#include "BKE_layer.h"
#include "BKE_main.h"
#include "BKE_node.h"
#include "BKE_workspace.h"
#include "BKE_object.h"

#include "DNA_group_types.h"
#include "DNA_ID.h"
#include "DNA_layer_types.h"
#include "DNA_object_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"
#include "DNA_windowmanager_types.h"
#include "DNA_workspace_types.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_debug.h"
#include "DEG_depsgraph_query.h"

#include "DRW_engine.h"

#include "RNA_access.h"

#include "MEM_guardedalloc.h"


/* prototype */
static void override_set_copy_data(struct OverrideSet *override_set_dst, struct OverrideSet *override_set_src);
static void override_set_free(struct OverrideSet *override_set);
static void object_bases_iterator_next(BLI_Iterator *iter, const int flag);


/*********************** Layer Collections and bases *************************/

static LayerCollection *layer_collection_add(ListBase *lb_parent, Collection *collection)
{
	LayerCollection *lc = MEM_callocN(sizeof(LayerCollection), "Collection Base");
	lc->collection = collection;
	BLI_addtail(lb_parent, lc);

	return lc;
}

static void layer_collection_free(ViewLayer *view_layer, LayerCollection *lc)
{
	if (lc == view_layer->active_collection) {
		view_layer->active_collection = view_layer->layer_collections.first;
	}

	for (LayerCollection *nlc = lc->layer_collections.first; nlc; nlc = nlc->next) {
		layer_collection_free(view_layer, nlc);
	}

	BLI_freelistN(&lc->layer_collections);
}

static Base *object_base_new(Object *ob)
{
	Base *base = MEM_callocN(sizeof(Base), "Object Base");
	base->object = ob;
	return base;
}

/********************************* View Layer ********************************/


/* RenderLayer */

/* Returns the default view layer to view in workspaces if there is
 * none linked to the workspace yet. */
ViewLayer *BKE_view_layer_default_view(const Scene *scene)
{
	for (ViewLayer *view_layer = scene->view_layers.first; view_layer; view_layer = view_layer->next) {
		if (!(view_layer->flag & VIEW_LAYER_RENDER)) {
			return view_layer;
		}
	}

	BLI_assert(scene->view_layers.first);
	return scene->view_layers.first;
}

/* Returns the default view layer to render if we need to render just one. */
ViewLayer *BKE_view_layer_default_render(const Scene *scene)
{
	for (ViewLayer *view_layer = scene->view_layers.first; view_layer; view_layer = view_layer->next) {
		if (view_layer->flag & VIEW_LAYER_RENDER) {
			return view_layer;
		}
	}

	BLI_assert(scene->view_layers.first);
	return scene->view_layers.first;
}

/**
 * Returns the ViewLayer to be used for drawing, outliner, and other context related areas.
 */
ViewLayer *BKE_view_layer_from_workspace_get(const struct Scene *scene, const struct WorkSpace *workspace)
{
	return BKE_workspace_view_layer_get(workspace, scene);
}

/**
 * This is a placeholder to know which areas of the code need to be addressed for the Workspace changes.
 * Never use this, you should either use BKE_view_layer_from_workspace_get or get ViewLayer explicitly.
 */
ViewLayer *BKE_view_layer_context_active_PLACEHOLDER(const Scene *scene)
{
	BLI_assert(scene->view_layers.first);
	return scene->view_layers.first;
}

static ViewLayer *view_layer_add(const char *name)
{
	if (!name) {
		name = DATA_("View Layer");
	}

	ViewLayer *view_layer = MEM_callocN(sizeof(ViewLayer), "View Layer");
	view_layer->flag = VIEW_LAYER_RENDER | VIEW_LAYER_FREESTYLE;

	BLI_strncpy_utf8(view_layer->name, name, sizeof(view_layer->name));

	/* Pure rendering pipeline settings. */
	view_layer->layflag = 0x7FFF;   /* solid ztra halo edge strand */
	view_layer->passflag = SCE_PASS_COMBINED | SCE_PASS_Z;
	view_layer->pass_alpha_threshold = 0.5f;
	BKE_freestyle_config_init(&view_layer->freestyle_config);

	return view_layer;
}

/**
 * Add a new view layer
 * by default, a view layer has the master collection
 */
ViewLayer *BKE_view_layer_add(Scene *scene, const char *name)
{
	ViewLayer *view_layer = view_layer_add(name);

	BLI_addtail(&scene->view_layers, view_layer);

	/* unique name */
	BLI_uniquename(
	        &scene->view_layers, view_layer, DATA_("ViewLayer"), '.',
	        offsetof(ViewLayer, name), sizeof(view_layer->name));

	BKE_layer_collection_sync(scene, view_layer);

	return view_layer;
}

void BKE_view_layer_free(ViewLayer *view_layer)
{
	BKE_view_layer_free_ex(view_layer, true);
}

/**
 * Free (or release) any data used by this ViewLayer.
 */
void BKE_view_layer_free_ex(ViewLayer *view_layer, const bool do_id_user)
{
	view_layer->basact = NULL;

	BLI_freelistN(&view_layer->object_bases);

	if (view_layer->object_bases_hash) {
		BLI_ghash_free(view_layer->object_bases_hash, NULL, NULL);
	}

	for (LayerCollection *lc = view_layer->layer_collections.first; lc; lc = lc->next) {
		layer_collection_free(view_layer, lc);
	}
	BLI_freelistN(&view_layer->layer_collections);

	for (ViewLayerEngineData *sled = view_layer->drawdata.first; sled; sled = sled->next) {
		if (sled->storage) {
			if (sled->free) {
				sled->free(sled->storage);
			}
			MEM_freeN(sled->storage);
		}
	}
	BLI_freelistN(&view_layer->drawdata);

	MEM_SAFE_FREE(view_layer->stats);

	BKE_freestyle_config_free(&view_layer->freestyle_config, do_id_user);

	if (view_layer->id_properties) {
		IDP_FreeProperty(view_layer->id_properties);
		MEM_freeN(view_layer->id_properties);
	}

	MEM_SAFE_FREE(view_layer->object_bases_array);

	for (OverrideSet *override_set = view_layer->override_sets.first; override_set; override_set = override_set->next) {
		override_set_free(override_set);
	}
	BLI_freelistN(&view_layer->override_sets);

	MEM_freeN(view_layer);
}

/**
 * Tag all the selected objects of a renderlayer
 */
void BKE_view_layer_selected_objects_tag(ViewLayer *view_layer, const int tag)
{
	for (Base *base = view_layer->object_bases.first; base; base = base->next) {
		if ((base->flag & BASE_SELECTED) != 0) {
			base->object->flag |= tag;
		}
		else {
			base->object->flag &= ~tag;
		}
	}
}

static bool find_scene_collection_in_scene_collections(ListBase *lb, const LayerCollection *lc)
{
	for (LayerCollection *lcn = lb->first; lcn; lcn = lcn->next) {
		if (lcn == lc) {
			return true;
		}
		if (find_scene_collection_in_scene_collections(&lcn->layer_collections, lc)) {
			return true;
		}
	}
	return false;
}

/**
 * Fallback for when a Scene has no camera to use
 *
 * \param view_layer: in general you want to use the same ViewLayer that is used
 * for depsgraph. If rendering you pass the scene active layer, when viewing in the viewport
 * you want to get ViewLayer from context.
 */
Object *BKE_view_layer_camera_find(ViewLayer *view_layer)
{
	for (Base *base = view_layer->object_bases.first; base; base = base->next) {
		if (base->object->type == OB_CAMERA) {
			return base->object;
		}
	}

	return NULL;
}

/**
 * Find the ViewLayer a LayerCollection belongs to
 */
ViewLayer *BKE_view_layer_find_from_collection(const Scene *scene, LayerCollection *lc)
{
	for (ViewLayer *view_layer = scene->view_layers.first; view_layer; view_layer = view_layer->next) {
		if (find_scene_collection_in_scene_collections(&view_layer->layer_collections, lc)) {
			return view_layer;
		}
	}

	return NULL;
}

/**
 * Return the view layer that owns the override set
 */
ViewLayer *BKE_view_layer_find_from_override_set(const Scene *scene, OverrideSet *override_set)
{
	for (ViewLayer *view_layer = scene->view_layers.first; view_layer; view_layer = view_layer->next) {
		if (BLI_findindex(&view_layer->override_sets, override_set) != -1) {
			return view_layer;
		}
	}
	return NULL;
}

/**
 * Return the view layer that owns the dynamic override property
 */
ViewLayer *BKE_view_layer_find_from_dynamic_override_property(const Scene *scene, DynamicOverrideProperty *dyn_prop)
{
	for (ViewLayer *view_layer = scene->view_layers.first; view_layer; view_layer = view_layer->next) {
		for (OverrideSet *override_set = view_layer->override_sets.first;
		     override_set != NULL;
		     override_set = override_set->next)
		{
			if (BLI_findindex(&override_set->scene_properties, dyn_prop) != -1) {
				return view_layer;
			}
			else if (BLI_findindex(&override_set->collection_properties, dyn_prop) != -1) {
				return view_layer;
			}
		}
	}
	return NULL;
}

/* Base */

static void view_layer_bases_hash_create(ViewLayer *view_layer)
{
	static ThreadMutex hash_lock = BLI_MUTEX_INITIALIZER;

	if (!view_layer->object_bases_hash) {
		BLI_mutex_lock(&hash_lock);

		view_layer->object_bases_hash = BLI_ghash_new(BLI_ghashutil_ptrhash, BLI_ghashutil_ptrcmp, __func__);

		for (Base *base = view_layer->object_bases.first; base; base = base->next) {
			BLI_ghash_insert(view_layer->object_bases_hash, base->object, base);
		}

		BLI_mutex_unlock(&hash_lock);
	}
}

Base *BKE_view_layer_base_find(ViewLayer *view_layer, Object *ob)
{
	if (!view_layer->object_bases_hash) {
		view_layer_bases_hash_create(view_layer);
	}

	return BLI_ghash_lookup(view_layer->object_bases_hash, ob);
}

void BKE_view_layer_base_deselect_all(ViewLayer *view_layer)
{
	Base *base;

	for (base = view_layer->object_bases.first; base; base = base->next) {
		base->flag &= ~BASE_SELECTED;
	}
}

void BKE_view_layer_base_select(struct ViewLayer *view_layer, Base *selbase)
{
	view_layer->basact = selbase;
	if ((selbase->flag & BASE_SELECTABLED) != 0) {
		selbase->flag |= BASE_SELECTED;
	}
}

/**************************** Copy View Layer and Layer Collections ***********************/

static void layer_collections_copy_data(ListBase *layer_collections_dst, const ListBase *layer_collections_src)
{
	BLI_duplicatelist(layer_collections_dst, layer_collections_src);

	LayerCollection *layer_collection_dst = layer_collections_dst->first;
	const LayerCollection *layer_collection_src = layer_collections_src->first;

	while (layer_collection_dst != NULL) {
		layer_collections_copy_data(
		        &layer_collection_dst->layer_collections,
		        &layer_collection_src->layer_collections);

		layer_collection_dst = layer_collection_dst->next;
		layer_collection_src = layer_collection_src->next;
	}
}

/**
 * Only copy internal data of ViewLayer from source to already allocated/initialized destination.
 *
 * \param flag  Copying options (see BKE_library.h's LIB_ID_COPY_... flags for more).
 */
void BKE_view_layer_copy_data(
        Scene *UNUSED(scene_dst), const Scene *UNUSED(scene_src),
        ViewLayer *view_layer_dst, const ViewLayer *view_layer_src,
        const int flag)
{
	if (view_layer_dst->id_properties != NULL) {
		view_layer_dst->id_properties = IDP_CopyProperty_ex(view_layer_dst->id_properties, flag);
	}
	BKE_freestyle_config_copy(&view_layer_dst->freestyle_config, &view_layer_src->freestyle_config, flag);

	view_layer_dst->stats = NULL;

	/* Clear temporary data. */
	BLI_listbase_clear(&view_layer_dst->drawdata);
	view_layer_dst->object_bases_array = NULL;
	view_layer_dst->object_bases_hash = NULL;

	/* Copy layer collections and object bases. */
	/* Inline 'BLI_duplicatelist' and update the active base. */
	BLI_listbase_clear(&view_layer_dst->object_bases);
	for (Base *base_src = view_layer_src->object_bases.first; base_src; base_src = base_src->next) {
		Base *base_dst = MEM_dupallocN(base_src);
		BLI_addtail(&view_layer_dst->object_bases, base_dst);
		if (view_layer_src->basact == base_src) {
			view_layer_dst->basact = base_dst;
		}
	}

	BLI_duplicatelist(&view_layer_dst->override_sets, &view_layer_src->override_sets);
	OverrideSet *override_set_dst, *override_set_src;
	for (override_set_dst = view_layer_dst->override_sets.first,
	     override_set_src = view_layer_src->override_sets.first;
	     override_set_dst != NULL;
	     override_set_dst = override_set_dst->next, override_set_src = override_set_src->next)
	{
		override_set_copy_data(override_set_dst, override_set_src);
	}

	layer_collections_copy_data(&view_layer_dst->layer_collections, &view_layer_src->layer_collections);

	// TODO: not always safe to free BKE_layer_collection_sync(scene_dst, view_layer_dst);
}

void BKE_view_layer_rename(Main *bmain, Scene *scene, ViewLayer *view_layer, const char *newname)
{
	char oldname[sizeof(view_layer->name)];

	BLI_strncpy(oldname, view_layer->name, sizeof(view_layer->name));

	BLI_strncpy_utf8(view_layer->name, newname, sizeof(view_layer->name));
	BLI_uniquename(&scene->view_layers, view_layer, DATA_("ViewLayer"), '.', offsetof(ViewLayer, name), sizeof(view_layer->name));

	if (scene->nodetree) {
		bNode *node;
		int index = BLI_findindex(&scene->view_layers, view_layer);

		for (node = scene->nodetree->nodes.first; node; node = node->next) {
			if (node->type == CMP_NODE_R_LAYERS && node->id == NULL) {
				if (node->custom1 == index)
					BLI_strncpy(node->name, view_layer->name, NODE_MAXSTR);
			}
		}
	}

	/* fix all the animation data and workspace which may link to this */
	BKE_animdata_fix_paths_rename_all(NULL, "view_layers", oldname, view_layer->name);
	BKE_workspace_view_layer_rename(bmain, scene, oldname, view_layer->name);

	/* Dependency graph uses view layer name based lookups. */
	DEG_id_tag_update(&scene->id, 0);
}

/* LayerCollection */

/**
 * Recursively get the collection for a given index
 */
static LayerCollection *collection_from_index(ListBase *lb, const int number, int *i)
{
	for (LayerCollection *lc = lb->first; lc; lc = lc->next) {
		if (*i == number) {
			return lc;
		}

		(*i)++;

		LayerCollection *lc_nested = collection_from_index(&lc->layer_collections, number, i);
		if (lc_nested) {
			return lc_nested;
		}
	}
	return NULL;
}

/**
 * Get the collection for a given index
 */
LayerCollection *BKE_layer_collection_from_index(ViewLayer *view_layer, const int index)
{
	int i = 0;
	return collection_from_index(&view_layer->layer_collections, index, &i);
}

/**
 * Get the active collection
 */
LayerCollection *BKE_layer_collection_get_active(ViewLayer *view_layer)
{
	return view_layer->active_collection;
}

/*
 * Activate collection
 */
bool BKE_layer_collection_activate(ViewLayer *view_layer, LayerCollection *lc)
{
	if (lc->flag & LAYER_COLLECTION_EXCLUDE) {
		return false;
	}

	view_layer->active_collection = lc;
	return true;
}

/**
 * Activate first parent collection
 */
LayerCollection *BKE_layer_collection_activate_parent(ViewLayer *view_layer, LayerCollection *lc)
{
	CollectionParent *parent = lc->collection->parents.first;

	if (parent) {
		lc = BKE_layer_collection_first_from_scene_collection(view_layer, parent->collection);
	}
	else {
		lc = NULL;
	}

	if (lc && (lc->flag & LAYER_COLLECTION_EXCLUDE)) {
		/* Don't activate excluded collections. */
		return BKE_layer_collection_activate_parent(view_layer, lc);
	}

	if (!lc) {
		lc = view_layer->layer_collections.first;
	}

	view_layer->active_collection = lc;
	return lc;
}

/**
 * Recursively get the count of collections
 */
static int collection_count(ListBase *lb)
{
	int i = 0;
	for (LayerCollection *lc = lb->first; lc; lc = lc->next) {
		i += collection_count(&lc->layer_collections) + 1;
	}
	return i;
}

/**
 * Get the total number of collections
 * (including all the nested collections)
 */
int BKE_layer_collection_count(ViewLayer *view_layer)
{
	return collection_count(&view_layer->layer_collections);
}

/**
 * Recursively get the index for a given collection
 */
static int index_from_collection(ListBase *lb, const LayerCollection *lc, int *i)
{
	for (LayerCollection *lcol = lb->first; lcol; lcol = lcol->next) {
		if (lcol == lc) {
			return *i;
		}

		(*i)++;

		int i_nested = index_from_collection(&lcol->layer_collections, lc, i);
		if (i_nested != -1) {
			return i_nested;
		}
	}
	return -1;
}

/**
 * Return -1 if not found
 */
int BKE_layer_collection_findindex(ViewLayer *view_layer, const LayerCollection *lc)
{
	int i = 0;
	return index_from_collection(&view_layer->layer_collections, lc, &i);
}

/*********************************** Syncing *********************************
 *
 * The layer collection tree mirrors the scene collection tree. Whenever that
 * changes we need to synchronize them so that there is a corresponding layer
 * collection for each collection. Note that the scene collection tree can
 * contain link or override collections, and so this is also called on .blend
 * file load to ensure any new or removed collections are synced.
 *
 * The view layer also contains a list of bases for each object that exists
 * in at least one layer collection. That list is also synchronized here, and
 * stores state like selection. */

static void layer_collection_sync(
        ViewLayer *view_layer, const ListBase *lb_scene,
        ListBase *lb_layer, ListBase *new_object_bases,
        int parent_exclude, int parent_restrict)
{
	/* TODO: support recovery after removal of intermediate collections, reordering, ..
	 * For local edits we can make editing operating do the appropriate thing, but for
	 * linking we can only sync after the fact. */

	/* Remove layer collections that no longer have a corresponding scene collection. */
	for (LayerCollection *lc = lb_layer->first; lc;) {
		/* Note ID remap can set lc->collection to NULL when deleting collections. */
		LayerCollection *lc_next = lc->next;
		Collection *collection = (lc->collection) ?
			BLI_findptr(lb_scene, lc->collection, offsetof(CollectionChild, collection)) : NULL;

		if (!collection) {
			/* Free recursively. */
			layer_collection_free(view_layer, lc);
			BLI_freelinkN(lb_layer, lc);
		}

		lc = lc_next;
	}

	/* Add layer collections for any new scene collections, and ensure order is the same. */
	ListBase new_lb_layer = {NULL, NULL};

	for (const CollectionChild *child = lb_scene->first; child; child = child->next) {
		Collection *collection = child->collection;
		LayerCollection *lc = BLI_findptr(lb_layer, collection, offsetof(LayerCollection, collection));

		if (lc) {
			BLI_remlink(lb_layer, lc);
			BLI_addtail(&new_lb_layer, lc);
		}
		else {
			lc = layer_collection_add(&new_lb_layer, collection);
			lc->flag = parent_exclude;
		}

		/* Collection restrict is inherited. */
		int child_restrict = parent_restrict;
		if (!(collection->flag & COLLECTION_IS_MASTER)) {
			child_restrict |= collection->flag;
		}

		/* Sync child collections. */
		layer_collection_sync(
		        view_layer, &collection->children,
		        &lc->layer_collections, new_object_bases,
		        lc->flag, child_restrict);

		/* Layer collection exclude is not inherited. */
		if (lc->flag & LAYER_COLLECTION_EXCLUDE) {
			continue;
		}

		/* Sync objects, except if collection was excluded. */
		for (CollectionObject *cob = collection->gobject.first; cob; cob = cob->next) {
			Base *base = BLI_ghash_lookup(view_layer->object_bases_hash, cob->ob);

			if (base) {
				/* Move from old base list to new base list. Base might have already
				 * been moved to the new base list and the first/last test ensure that
				 * case also works. */
				if (!ELEM(base, new_object_bases->first, new_object_bases->last)) {
					BLI_remlink(&view_layer->object_bases, base);
					BLI_addtail(new_object_bases, base);
				}
			}
			else {
				/* Create new base. */
				base = object_base_new(cob->ob);
				BLI_addtail(new_object_bases, base);
				BLI_ghash_insert(view_layer->object_bases_hash, base->object, base);
			}

			if ((child_restrict & COLLECTION_RESTRICT_VIEW) == 0) {
				base->flag |= BASE_VISIBLED | BASE_VISIBLE_VIEWPORT;

				if ((child_restrict & COLLECTION_RESTRICT_SELECT) == 0) {
					base->flag |= BASE_SELECTABLED;
				}
			}
			if ((child_restrict & COLLECTION_RESTRICT_RENDER) == 0) {
				base->flag |= BASE_VISIBLE_RENDER;
			}
		}
	}

	/* Replace layer collection list with new one. */
	*lb_layer = new_lb_layer;
	BLI_assert(BLI_listbase_count(lb_scene) == BLI_listbase_count(lb_layer));
}

/**
 * Update view layer collection tree from collections used in the scene.
 * This is used when collections are removed or added, both while editing
 * and on file loaded in case linked data changed or went missing.
 */
void BKE_layer_collection_sync(const Scene *scene, ViewLayer *view_layer)
{
	if (!scene->master_collection) {
		/* Happens for old files that don't have versioning applied yet. */
		return;
	}

	/* Free cache. */
	MEM_SAFE_FREE(view_layer->object_bases_array);

	/* Create object to base hash if it does not exist yet. */
	if (!view_layer->object_bases_hash) {
		view_layer_bases_hash_create(view_layer);
	}

	/* Clear visible and selectable flags to be reset. */
	for (Base *base = view_layer->object_bases.first; base; base = base->next) {
		base->flag &= ~(BASE_VISIBLED | BASE_SELECTABLED | BASE_VISIBLE_VIEWPORT | BASE_VISIBLE_RENDER);
	}

	/* Generate new layer connections and object bases when collections changed. */
	CollectionChild child = {NULL, NULL, scene->master_collection};
	const ListBase collections = {&child, &child};
	ListBase new_object_bases = {NULL, NULL};

	const int parent_exclude = 0, parent_restrict = 0;
	layer_collection_sync(
	        view_layer, &collections,
	        &view_layer->layer_collections, &new_object_bases,
	        parent_exclude, parent_restrict);

	/* Any remaning object bases are to be removed. */
	for (Base *base = view_layer->object_bases.first; base; base = base->next) {
		if (view_layer->basact == base) {
			view_layer->basact = NULL;
		}

		BLI_ghash_remove(view_layer->object_bases_hash, base->object, NULL, NULL);
	}

	BLI_freelistN(&view_layer->object_bases);
	view_layer->object_bases = new_object_bases;

	/* Always set a valid active collection. */
	LayerCollection *active = view_layer->active_collection;

	if (active && (active->flag & LAYER_COLLECTION_EXCLUDE)) {
		BKE_layer_collection_activate_parent(view_layer, active);
	}
	else if (active == NULL) {
		view_layer->active_collection = view_layer->layer_collections.first;
	}
}

void BKE_scene_collection_sync(const Scene *scene)
{
	for (ViewLayer *view_layer = scene->view_layers.first; view_layer; view_layer = view_layer->next) {
		BKE_layer_collection_sync(scene, view_layer);
	}
}

void BKE_main_collection_sync(const Main *bmain)
{
	/* TODO: if a single collection changed, figure out which
	 * scenes it belongs to and only update those. */

	/* TODO: optimize for file load so only linked collections get checked? */

	for (const Scene *scene = bmain->scene.first; scene; scene = scene->id.next) {
		BKE_scene_collection_sync(scene);
	}
}

void BKE_main_collection_sync_remap(const Main *bmain)
{
	/* On remapping of object or collection pointers free caches. */
	/* TODO: try to make this faster */

	for (const Scene *scene = bmain->scene.first; scene; scene = scene->id.next) {
		for (ViewLayer *view_layer = scene->view_layers.first; view_layer; view_layer = view_layer->next) {
			MEM_SAFE_FREE(view_layer->object_bases_array);

			if (view_layer->object_bases_hash) {
				BLI_ghash_free(view_layer->object_bases_hash, NULL, NULL);
				view_layer->object_bases_hash = NULL;
			}
		}
	}

	for (Collection *collection = bmain->collection.first; collection; collection = collection->id.next) {
		BKE_collection_object_cache_free(collection);
	}

	BKE_main_collection_sync(bmain);
}

/* ---------------------------------------------------------------------- */

/**
 * Select all the objects of this layer collection
 *
 * It also select the objects that are in nested collections.
 * \note Recursive
 */
bool BKE_layer_collection_objects_select(ViewLayer *view_layer, LayerCollection *lc, bool deselect)
{
	if (lc->collection->flag & COLLECTION_RESTRICT_SELECT) {
		return false;
	}

	bool changed = false;

	if (!(lc->flag & LAYER_COLLECTION_EXCLUDE)) {
		for (CollectionObject *cob = lc->collection->gobject.first; cob; cob = cob->next) {
			Base *base = BKE_view_layer_base_find(view_layer, cob->ob);

			if (base) {
				if (deselect) {
					if (base->flag & BASE_SELECTED) {
						base->flag &= ~BASE_SELECTED;
						changed = true;
					}
				}
				else {
					if ((base->flag & BASE_SELECTABLED) && !(base->flag & BASE_SELECTED)) {
						base->flag |= BASE_SELECTED;
						changed = true;
					}
				}
			}
		}
	}

	for (LayerCollection *iter = lc->layer_collections.first; iter; iter = iter->next) {
		changed |= BKE_layer_collection_objects_select(view_layer, iter, deselect);
	}

	return changed;
}

/* ---------------------------------------------------------------------- */

static LayerCollection *find_layer_collection_by_scene_collection(LayerCollection *lc, const Collection *collection)
{
	if (lc->collection == collection) {
		return lc;
	}

	for (LayerCollection *nlc = lc->layer_collections.first; nlc; nlc = nlc->next) {
		LayerCollection *found = find_layer_collection_by_scene_collection(nlc, collection);
		if (found) {
			return found;
		}
	}
	return NULL;
}

/**
 * Return the first matching LayerCollection in the ViewLayer for the Collection.
 */
LayerCollection *BKE_layer_collection_first_from_scene_collection(ViewLayer *view_layer, const Collection *collection)
{
	for (LayerCollection *layer_collection = view_layer->layer_collections.first;
	     layer_collection != NULL;
	     layer_collection = layer_collection->next)
	{
		LayerCollection *found = find_layer_collection_by_scene_collection(layer_collection, collection);

		if (found != NULL) {
			return found;
		}
	}
	return NULL;
}

/**
 * See if view layer has the scene collection linked directly, or indirectly (nested)
 */
bool BKE_view_layer_has_collection(ViewLayer *view_layer, const Collection *collection)
{
	return BKE_layer_collection_first_from_scene_collection(view_layer, collection) != NULL;
}

/**
 * See if the object is in any of the scene layers of the scene
 */
bool BKE_scene_has_object(Scene *scene, Object *ob)
{
	for (ViewLayer *view_layer = scene->view_layers.first; view_layer; view_layer = view_layer->next) {
		Base *base = BKE_view_layer_base_find(view_layer, ob);
		if (base) {
			return true;
		}
	}
	return false;
}

/* ---------------------------------------------------------------------- */
/* Override */

/**
 * Add a new datablock override
 */
void BKE_override_view_layer_datablock_add(
        ViewLayer *view_layer, int id_type, const char *data_path, const ID *owner_id)
{
	UNUSED_VARS(view_layer, id_type, data_path, owner_id);
	TODO_LAYER_OVERRIDE;
}

/**
 * Add a new int override
 */
void BKE_override_view_layer_int_add(
        ViewLayer *view_layer, int id_type, const char *data_path, const int value)
{
	UNUSED_VARS(view_layer, id_type, data_path, value);
	TODO_LAYER_OVERRIDE;
}

/**
 * Add a new boolean override
 */
void BKE_override_layer_collection_boolean_add(
        struct LayerCollection *layer_collection, int id_type, const char *data_path, const bool value)
{
	UNUSED_VARS(layer_collection, id_type, data_path, value);
	TODO_LAYER_OVERRIDE;
}

/** \} */

/* Iterators */

/* -------------------------------------------------------------------- */
/** \name Public Dynamic Overrides
 * \{ */

static void dynamic_property_copy_data(ListBase *lb_dst, ListBase *lb_src)
{
	BLI_duplicatelist(lb_dst, lb_src);

	DynamicOverrideProperty *dyn_prop_dst;
	DynamicOverrideProperty *dyn_prop_src;
	for (dyn_prop_dst = lb_dst->first, dyn_prop_src = lb_src->first;
	     dyn_prop_dst != NULL;
	     dyn_prop_dst = dyn_prop_dst->next, dyn_prop_src = dyn_prop_src->next)
	{
		if (dyn_prop_src->rna_path != NULL) {
			dyn_prop_dst->rna_path = MEM_dupallocN(dyn_prop_src->rna_path);
		}
		if (dyn_prop_src->data.str != NULL) {
			dyn_prop_dst->data.str = MEM_dupallocN(dyn_prop_src->data.str);
		}
		BLI_duplicatelist(&dyn_prop_dst->data_path, &dyn_prop_src->data_path);
	}
}

static void override_set_copy_data(OverrideSet *override_set_dst, OverrideSet *override_set_src)
{
	BLI_strncpy(override_set_dst->name, override_set_src->name, sizeof(override_set_dst->name));
	override_set_dst->flag = override_set_src->flag;
	BLI_duplicatelist(&override_set_dst->affected_collections, &override_set_src->affected_collections);
	dynamic_property_copy_data(&override_set_dst->scene_properties, &override_set_src->scene_properties);
	dynamic_property_copy_data(&override_set_dst->collection_properties, &override_set_src->collection_properties);
}

static void override_set_property_free(DynamicOverrideProperty *dyn_prop)
{
	MEM_SAFE_FREE(dyn_prop->rna_path);
	MEM_SAFE_FREE(dyn_prop->data.str);
	BLI_freelistN(&dyn_prop->data_path);
}

static void override_set_properties_free(ListBase *properties)
{
	for (DynamicOverrideProperty *dyn_prop = properties->first; dyn_prop; dyn_prop = dyn_prop->next) {
		override_set_property_free(dyn_prop);
	}
}

static void override_set_free(OverrideSet *override_set)
{
	BLI_freelistN(&override_set->affected_collections);
	override_set_properties_free(&override_set->scene_properties);
	BLI_freelistN(&override_set->scene_properties);
	override_set_properties_free(&override_set->collection_properties);
	BLI_freelistN(&override_set->collection_properties);
}

struct OverrideSet *BKE_view_layer_override_set_add(struct ViewLayer *view_layer, const char *name)
{
	OverrideSet *override_set = MEM_callocN(sizeof(OverrideSet), __func__);
	override_set->flag = DYN_OVERRIDE_SET_USE;

	BLI_strncpy_utf8(override_set->name, name, sizeof(override_set->name));
	BLI_uniquename(&view_layer->override_sets,
	               override_set,
	               DATA_("OverrideSet"),
	               '.',
	               offsetof(OverrideSet, name),
	               sizeof(override_set->name));

	BLI_addtail(&view_layer->override_sets, override_set);
	view_layer->active_override_set = BLI_listbase_count(&view_layer->override_sets) - 1;
	return override_set;
}

bool BKE_view_layer_override_set_remove(struct ViewLayer *view_layer, struct OverrideSet *override_set)
{
	const int override_set_index = BLI_findindex(&view_layer->override_sets, override_set);

	if (override_set_index == -1) {
		return false;
	}

	BLI_remlink(&view_layer->override_sets, override_set);
	override_set_free(override_set);
	MEM_freeN(override_set);

	if (view_layer->active_override_set > override_set_index) {
		view_layer->active_override_set -= 1;
	}
	else if (view_layer->active_override_set == override_set_index) {
		if (override_set_index == BLI_listbase_count(&view_layer->override_sets)) {
			view_layer->active_override_set = MAX2(0, override_set_index - 1);
		}
	}

	return true;
}

/**
 * Add an existent collection to the affected collection list.
 * If collection already exists it returns false.
 */
bool BKE_view_layer_override_set_collection_link(OverrideSet *override_set, Collection *collection)
{
	/* We don't support duplicated collections in the override set. */
	if (BLI_findptr(&override_set->affected_collections, collection, offsetof(LinkData, data)) != NULL) {
		return false;
	};

	BLI_addtail(&override_set->affected_collections, BLI_genericNodeN(collection));
	return true;
}

/**
 * Remove collection from override set
 * If collection was not there it returns false.
 */
bool BKE_view_layer_override_set_collection_unlink(struct OverrideSet *override_set, struct Collection *collection)
{
	LinkData *link = BLI_findptr(&override_set->affected_collections, collection, offsetof(LinkData, data));
	if (link == NULL) {
		return false;
	}

	const int collection_index = BLI_findindex(&override_set->affected_collections, link);

	BLI_remlink(&override_set->affected_collections, link);
	MEM_freeN(link);

	if (override_set->active_affected_collection > collection_index) {
		override_set->active_affected_collection -= 1;
	}
	else if (override_set->active_affected_collection == collection_index) {
		if (collection_index == BLI_listbase_count(&override_set->affected_collections)) {
			override_set->active_affected_collection = MAX2(0, collection_index - 1);
		}
	}
	return true;
}

/**
 * Remove collection from all the override sets.
 */
void BKE_dynamic_overrides_remove_collection(Main *bmain, Collection *old_collection)
{
	for (Scene *scene = bmain->scene.first; scene; scene = scene->id.next) {
		for (ViewLayer *view_layer = scene->view_layers.first; view_layer; view_layer = view_layer->next) {
			for (OverrideSet *override_set = view_layer->override_sets.first;
			     override_set != NULL;
			     override_set = override_set->next)
			{
				BKE_view_layer_override_set_collection_unlink(override_set, old_collection);
			}
		}
	}
}

DynamicOverrideProperty *BKE_view_layer_override_property_add(
        OverrideSet *override_set,
        PointerRNA *ptr,
        PropertyRNA *prop,
        const int index)
{
	ID *owner_id = ptr->id.data;
	eDynamicOverridePropertyType property_type;
	const short id_type = GS(owner_id->name);

	switch (id_type) {
		case ID_OB:
		case ID_ME:
		case ID_MA:
			property_type = DYN_OVERRIDE_PROP_TYPE_COLLECTION;
			break;
		case ID_SCE:
		case ID_WO:
			property_type = DYN_OVERRIDE_PROP_TYPE_SCENE;
			break;
		default:
			BLI_assert("!undefined dynamic assert type");
			return NULL;
	}

	char *rna_path_str = RNA_path_from_ID_to_property_index(ptr, prop, 0, index);
	if (rna_path_str == NULL) {
		printf("%s: could not get valid RNA path!\n", __func__);
		return NULL;
	}

	const int array_len = RNA_property_array_length(ptr, prop);
	if (array_len > ARRAY_SIZE(((DynamicOverrideProperty *)NULL)->data.i)) {
		/* TODO Use a define for supported array length. */
		BLI_assert(!"Trying to dynamic-override an array longer than supported!");
		return NULL;
	}

	DynamicOverrideProperty *dyn_prop = MEM_callocN(sizeof(DynamicOverrideProperty), __func__);
	dyn_prop->flag = DYN_OVERRIDE_PROP_USE;
	dyn_prop->operation = IDOVERRIDESTATIC_OP_REPLACE;
	/* TODO: We want to store the id only when the rna path is only relevant to
	 * this particular object (e.g., modifiers of an object) .*/
	dyn_prop->root = owner_id;
	dyn_prop->id_type = id_type;
	dyn_prop->property_type = property_type;
	dyn_prop->rna_path = rna_path_str;
	dyn_prop->array_len = array_len;

	const bool is_array = RNA_property_array_check(prop);

	/* TODO handle array. */
	switch (RNA_property_type(prop)) {
		case PROP_BOOLEAN:
			if (is_array) {
				RNA_property_boolean_get_array(ptr, prop, dyn_prop->data.i);
			}
			else {
				dyn_prop->data.i[0] = RNA_property_boolean_get(ptr, prop);
			}
			break;
		case PROP_INT:
			if (is_array) {
				RNA_property_int_get_array(ptr, prop, dyn_prop->data.i);
			}
			else {
				dyn_prop->data.i[0] = RNA_property_int_get(ptr, prop);
			}
			break;
		case PROP_FLOAT:
			if (is_array) {
				RNA_property_float_get_array(ptr, prop, dyn_prop->data.f);
			}
			else {
				dyn_prop->data.f[0] = RNA_property_float_get(ptr, prop);
			}
			break;
		case PROP_STRING:
			dyn_prop->data.str = RNA_property_string_get_alloc(ptr, prop, NULL, 0, NULL);
			break;
		case PROP_ENUM:
			dyn_prop->data.i[0] = RNA_property_enum_get(ptr, prop);
			break;
		case PROP_POINTER:
		{
			PointerRNA poin = RNA_property_pointer_get(ptr, prop);
			BLI_assert(RNA_struct_is_ID(poin.type));
			dyn_prop->data.id = poin.id.data;
			break;
		}
		case PROP_COLLECTION:
		default:
			BLI_assert(!"Should never happen - dyn");
			break;
	}

	/* TODO - data_path for depsgraph. */

	if (property_type == DYN_OVERRIDE_PROP_TYPE_SCENE) {
		BLI_addtail(&override_set->scene_properties, dyn_prop);
	}
	else {
		BLI_addtail(&override_set->collection_properties, dyn_prop);
	}

	return dyn_prop;
}

bool BKE_view_layer_override_property_remove(
        OverrideSet *override_set,
        DynamicOverrideProperty *dyn_prop)
{
	bool ok = BLI_remlink_safe(&override_set->scene_properties, dyn_prop);

	if (ok == false) {
		ok = BLI_remlink_safe(&override_set->collection_properties, dyn_prop);
	}

	if (ok == false) {
		/* Something went wrong. */
		return false;
	}

	override_set_property_free(dyn_prop);
	MEM_freeN(dyn_prop);
	return true;
}

void BKE_dynamic_override_apply(const struct Depsgraph *depsgraph, ID *id)
{
	const ID_Type id_type = GS(id->name);
	if (ELEM(id_type, ID_SCE, ID_OB) == false) {
		return;
	}

	ViewLayer *view_layer = DEG_get_evaluated_view_layer(depsgraph);
	for (OverrideSet *override_set = view_layer->override_sets.first;
	     override_set != NULL;
	     override_set = override_set->next)
	{
		if ((override_set->flag & DYN_OVERRIDE_SET_USE) == 0) {
			continue;
		}

		if (id_type == ID_SCE) {
#if 1
			/** Apply all the scene properties. */
			for (DynamicOverrideProperty *dyn_prop = override_set->scene_properties.first;
				 dyn_prop != NULL;
				 dyn_prop = dyn_prop->next)
			{
				if ((dyn_prop->flag & DYN_OVERRIDE_PROP_USE) == 0) {
					continue;
				}

				PointerRNA ptr;
				RNA_id_pointer_create(id, &ptr);
				RNA_struct_dynamic_override_apply(&ptr, dyn_prop);
			}
#endif
		}
		else {
#if 1
			/** Check if object is in one of the affected collections.
			 *  If it is, apply all the overrides for the object and its material, mesh, ...
			 **/
			for (DynamicOverrideProperty *dyn_prop = override_set->collection_properties.first;
				 dyn_prop != NULL;
				 dyn_prop = dyn_prop->next)
			{
				if ((dyn_prop->flag & DYN_OVERRIDE_PROP_USE) == 0) {
					continue;
				}

				/* If object is in collection ... */
				PointerRNA ptr;
				RNA_id_pointer_create(id, &ptr);
				RNA_struct_dynamic_override_apply(&ptr, dyn_prop);
			}
#endif
		}
	}
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Private Iterator Helpers
 * \{ */

static void object_bases_iterator_begin(BLI_Iterator *iter, void *data_in, const int flag)
{
	ViewLayer *view_layer = data_in;
	Base *base = view_layer->object_bases.first;

	/* when there are no objects */
	if (base ==  NULL) {
		iter->valid = false;
		return;
	}

	iter->data = base;

	if ((base->flag & flag) == 0) {
		object_bases_iterator_next(iter, flag);
	}
	else {
		iter->current = base;
	}
}

static void object_bases_iterator_next(BLI_Iterator *iter, const int flag)
{
	Base *base = ((Base *)iter->data)->next;

	while (base) {
		if ((base->flag & flag) != 0) {
			iter->current = base;
			iter->data = base;
			return;
		}
		base = base->next;
	}

	iter->valid = false;
}

static void objects_iterator_begin(BLI_Iterator *iter, void *data_in, const int flag)
{
	object_bases_iterator_begin(iter, data_in, flag);

	if (iter->valid) {
		iter->current = ((Base *)iter->current)->object;
	}
}

static void objects_iterator_next(BLI_Iterator *iter, const int flag)
{
	object_bases_iterator_next(iter, flag);

	if (iter->valid) {
		iter->current = ((Base *)iter->current)->object;
	}
}

/* -------------------------------------------------------------------- */
/** \name BKE_view_layer_selected_objects_iterator
 * See: #FOREACH_SELECTED_OBJECT_BEGIN
 * \{ */

void BKE_view_layer_selected_objects_iterator_begin(BLI_Iterator *iter, void *data_in)
{
	objects_iterator_begin(iter, data_in, BASE_SELECTED);
}

void BKE_view_layer_selected_objects_iterator_next(BLI_Iterator *iter)
{
	objects_iterator_next(iter, BASE_SELECTED);
}

void BKE_view_layer_selected_objects_iterator_end(BLI_Iterator *UNUSED(iter))
{
	/* do nothing */
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name BKE_view_layer_visible_objects_iterator
 * \{ */

void BKE_view_layer_visible_objects_iterator_begin(BLI_Iterator *iter, void *data_in)
{
	objects_iterator_begin(iter, data_in, BASE_VISIBLED);
}

void BKE_view_layer_visible_objects_iterator_next(BLI_Iterator *iter)
{
	objects_iterator_next(iter, BASE_VISIBLED);
}

void BKE_view_layer_visible_objects_iterator_end(BLI_Iterator *UNUSED(iter))
{
	/* do nothing */
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name BKE_view_layer_selected_editable_objects_iterator
 * \{ */

void BKE_view_layer_selected_editable_objects_iterator_begin(BLI_Iterator *iter, void *data_in)
{
	objects_iterator_begin(iter, data_in, BASE_SELECTED);
	if (iter->valid) {
		if (BKE_object_is_libdata((Object *)iter->current) == false) {
			// First object is valid (selectable and not libdata) -> all good.
			return;
		}
		else {
			// Object is selectable but not editable -> search for another one.
			BKE_view_layer_selected_editable_objects_iterator_next(iter);
		}
	}
}

void BKE_view_layer_selected_editable_objects_iterator_next(BLI_Iterator *iter)
{
	// Search while there are objects and the one we have is not editable (editable = not libdata).
	do {
		objects_iterator_next(iter, BASE_SELECTED);
	} while (iter->valid && BKE_object_is_libdata((Object *)iter->current) != false);
}

void BKE_view_layer_selected_editable_objects_iterator_end(BLI_Iterator *UNUSED(iter))
{
	/* do nothing */
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name BKE_view_layer_selected_bases_iterator
 * \{ */

void BKE_view_layer_selected_bases_iterator_begin(BLI_Iterator *iter, void *data_in)
{
	object_bases_iterator_begin(iter, data_in, BASE_SELECTED);
}

void BKE_view_layer_selected_bases_iterator_next(BLI_Iterator *iter)
{
	object_bases_iterator_next(iter, BASE_SELECTED);
}

void BKE_view_layer_selected_bases_iterator_end(BLI_Iterator *UNUSED(iter))
{
	/* do nothing */
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name BKE_view_layer_visible_bases_iterator
 * \{ */

void BKE_view_layer_visible_bases_iterator_begin(BLI_Iterator *iter, void *data_in)
{
	object_bases_iterator_begin(iter, data_in, BASE_VISIBLED);
}

void BKE_view_layer_visible_bases_iterator_next(BLI_Iterator *iter)
{
	object_bases_iterator_next(iter, BASE_VISIBLED);
}

void BKE_view_layer_visible_bases_iterator_end(BLI_Iterator *UNUSED(iter))
{
	/* do nothing */
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name BKE_view_layer_renderable_objects_iterator
 * \{ */

void BKE_view_layer_renderable_objects_iterator_begin(BLI_Iterator *iter, void *data_in)
{
	struct ObjectsRenderableIteratorData *data = data_in;

	/* Tag objects to prevent going over the same object twice. */
	for (Scene *scene = data->scene; scene; scene = scene->set) {
		for (ViewLayer *view_layer = scene->view_layers.first; view_layer; view_layer = view_layer->next) {
			for (Base *base = view_layer->object_bases.first; base; base = base->next) {
				 base->object->id.flag |= LIB_TAG_DOIT;
			}
		}
	}

	ViewLayer *view_layer = data->scene->view_layers.first;
	data->iter.view_layer = view_layer;

	data->base_temp.next = view_layer->object_bases.first;
	data->iter.base = &data->base_temp;

	data->iter.set = NULL;

	iter->data = data_in;
	BKE_view_layer_renderable_objects_iterator_next(iter);
}

void BKE_view_layer_renderable_objects_iterator_next(BLI_Iterator *iter)
{
	/* Set it early in case we need to exit and we are running from within a loop. */
	iter->skip = true;

	struct ObjectsRenderableIteratorData *data = iter->data;
	Base *base = data->iter.base->next;

	/* There is still a base in the current scene layer. */
	if (base != NULL) {
		Object *ob = base->object;

		/* We need to set the iter.base even if the rest fail otherwise
		 * we keep checking the exactly same base over and over again. */
		data->iter.base = base;

		if (ob->id.flag & LIB_TAG_DOIT) {
			ob->id.flag &= ~LIB_TAG_DOIT;

			if ((base->flag & BASE_VISIBLED) != 0) {
				iter->skip = false;
				iter->current = ob;
			}
		}
		return;
	}

	/* Time to go to the next scene layer. */
	if (data->iter.set == NULL) {
		while ((data->iter.view_layer = data->iter.view_layer->next)) {
			ViewLayer *view_layer = data->iter.view_layer;
			if (view_layer->flag & VIEW_LAYER_RENDER) {
				data->base_temp.next = view_layer->object_bases.first;
				data->iter.base = &data->base_temp;
				return;
			}
		}

		/* Setup the "set" for the next iteration. */
		data->scene_temp.set = data->scene;
		data->iter.set = &data->scene_temp;
		return;
	}

	/* Look for an object in the next set. */
	while ((data->iter.set = data->iter.set->set)) {
		ViewLayer *view_layer = BKE_view_layer_default_render(data->iter.set);
		data->base_temp.next = view_layer->object_bases.first;
		data->iter.base = &data->base_temp;
		return;
	}

	iter->valid = false;
}

void BKE_view_layer_renderable_objects_iterator_end(BLI_Iterator *UNUSED(iter))
{
	/* Do nothing - iter->data was static allocated, we can't free it. */
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name BKE_view_layer_bases_in_mode_iterator
 * \{ */

void BKE_view_layer_bases_in_mode_iterator_begin(BLI_Iterator *iter, void *data_in)
{
	struct ObjectsInModeIteratorData *data = data_in;
	Base *base = data->base_active;

	/* when there are no objects */
	if (base == NULL) {
		iter->valid = false;
		return;
	}
	iter->data = data_in;
	iter->current = base;
}

void BKE_view_layer_bases_in_mode_iterator_next(BLI_Iterator *iter)
{
	struct ObjectsInModeIteratorData *data = iter->data;
	Base *base = iter->current;

	if (base == data->base_active) {
		/* first step */
		base = data->view_layer->object_bases.first;
		if (base == data->base_active) {
			base = base->next;
		}
	}
	else {
		base = base->next;
	}

	while (base) {
		if ((base->flag & BASE_SELECTED) != 0 &&
		    (base->object->type == data->base_active->object->type) &&
		    (base != data->base_active) &&
		    (base->object->mode & data->object_mode))
		{
			iter->current = base;
			return;
		}
		base = base->next;
	}
	iter->valid = false;
}

void BKE_view_layer_bases_in_mode_iterator_end(BLI_Iterator *UNUSED(iter))
{
	/* do nothing */
}

/** \} */

/* Evaluation  */

void BKE_layer_eval_view_layer(
        struct Depsgraph *depsgraph,
        struct Scene *UNUSED(scene),
        ViewLayer *view_layer)
{
	DEG_debug_print_eval(depsgraph, __func__, view_layer->name, view_layer);

	/* Set visibility based on depsgraph mode. */
	const eEvaluationMode mode = DEG_get_mode(depsgraph);
	const int base_flag = (mode == DAG_EVAL_VIEWPORT) ? BASE_VISIBLE_VIEWPORT : BASE_VISIBLE_RENDER;

	for (Base *base = view_layer->object_bases.first; base != NULL; base = base->next) {
		if (base->flag & base_flag) {
			base->flag |= BASE_VISIBLED;
		}
		else {
			base->flag &= ~BASE_VISIBLED;
		}
	}

	/* TODO(sergey): Is it always required? */
	view_layer->flag |= VIEW_LAYER_ENGINE_DIRTY;

	/* Create array of bases, for fast index-based lookup. */
	const int num_object_bases = BLI_listbase_count(&view_layer->object_bases);
	MEM_SAFE_FREE(view_layer->object_bases_array);
	view_layer->object_bases_array = MEM_malloc_arrayN(
	        num_object_bases, sizeof(Base *), "view_layer->object_bases_array");
	int base_index = 0;
	for (Base *base = view_layer->object_bases.first; base; base = base->next) {
		/* if base is not selectabled, clear select. */
		if ((base->flag & BASE_SELECTABLED) == 0) {
			base->flag &= ~BASE_SELECTED;
		}
		/* Store base in the array. */
		view_layer->object_bases_array[base_index++] = base;
	}
}

void BKE_layer_eval_view_layer_indexed(
        struct Depsgraph *depsgraph,
        struct Scene *scene,
        int view_layer_index)
{
	BLI_assert(view_layer_index >= 0);
	ViewLayer *view_layer = BLI_findlink(&scene->view_layers, view_layer_index);
	BLI_assert(view_layer != NULL);
	BKE_layer_eval_view_layer(depsgraph, scene, view_layer);
}
