/*
 *      treebrowser.c
 *
 *      Copyright 2010 Adrian Dimitrov <dimitrov.adrian@gmail.com>
 */

#include <glib/gstdio.h>

#include "geanyplugin.h"

/* These items are set by Geany before plugin_init() is called. */
GeanyPlugin 				*geany_plugin;
GeanyData 					*geany_data;
GeanyFunctions 				*geany_functions;

static gint 				page_number 				= 0;
static GtkTreeStore 		*treestore;
static GtkWidget 			*treeview;
static GtkWidget 			*sidebar_vbox;
static GtkWidget 			*sidebar_vbox_bars;
static GtkWidget 			*filter;
static GtkWidget 			*addressbar;
static gchar 				*addressbar_last_address 	= NULL;

static GtkWidget 			*menu_showbars;

static GtkTreeViewColumn 	*treeview_column_icon, *treeview_column_text;
static GtkCellRenderer 		*render_icon, *render_text;


/* ------------------
 *  CONFIG VARS
 * ------------------ */

static gchar 				*CONFIG_FILE 				= NULL;
static gchar 				*CONFIG_OPEN_EXTERNAL_CMD 	= "nautilus '%d'";
static gint 				CONFIG_INITIAL_DIR_DEEP 	= 1;
static gboolean 			CONFIG_REVERSE_FILTER 		= FALSE;
static gboolean 			CONFIG_ONE_CLICK_CHDOC 		= FALSE;
static gboolean 			CONFIG_SHOW_HIDDEN_FILES 	= FALSE;
static gboolean 			CONFIG_SHOW_BARS			= TRUE;
static gboolean 			CONFIG_CHROOT_ON_DCLICK		= FALSE;
static gboolean 			CONFIG_FOLLOW_CURRENT_DOC 	= TRUE;


/* ------------------
 * TREEVIEW STRUCT
 * ------------------ */
enum
{
	TREEBROWSER_COLUMNC 								= 3,

	TREEBROWSER_COLUMN_ICON								= 0,
	TREEBROWSER_COLUMN_NAME								= 1,
	TREEBROWSER_COLUMN_URI								= 2,

	TREEBROWSER_RENDER_ICON								= 0,
	TREEBROWSER_RENDER_TEXT								= 1
};


/* ------------------
 * PLUGIN INFO
 * ------------------ */
PLUGIN_VERSION_CHECK(147)
PLUGIN_SET_INFO(_("Tree Browser"), _("Treeview filebrowser plugin."), "0.1" , "Adrian Dimitrov (dimitrov.adrian@gmail.com)")


/* ------------------
 * PREDEFINES
 * ------------------ */
#define foreach_slist_free(node, list) for (node = list, list = NULL; g_slist_free_1(list), node != NULL; list = node, node = node->next)

static GList*
_gtk_cell_layout_get_cells(GtkTreeViewColumn *column)
{
#if GTK_CHECK_VERSION(2, 12, 0)
	return gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(column));
#else
	return gtk_tree_view_column_get_cell_renderers(column);
#endif
}


/* ------------------
 * PROTOTYPES
 * ------------------ */
static void 	treebrowser_browse(gchar *directory, gpointer parent, gint deep_limit);
static void 	gtk_tree_store_iter_clear_nodes(gpointer iter, gboolean delete_root);


/* ------------------
 * TREEBROWSER CORE FUNCTIONS
 * ------------------ */

static gboolean
check_filtered(const gchar *base_name)
{
	gchar		**filters;
	gint 		i;
	gboolean 	temporary_reverse = FALSE;

	if (! NZV(gtk_entry_get_text(GTK_ENTRY(filter))))
		return TRUE;

	filters = g_strsplit(gtk_entry_get_text(GTK_ENTRY(filter)), ";", 0);

	if (utils_str_equal(filters[0], "!") == TRUE)
	{
		temporary_reverse = TRUE;
		i = 1;
	}
	else
		i = 0;

	for (; filters[i]; i++)
		if (utils_str_equal(base_name, "*") || g_pattern_match_simple(filters[i], base_name))
			return CONFIG_REVERSE_FILTER || temporary_reverse ? FALSE : TRUE;

	return CONFIG_REVERSE_FILTER || temporary_reverse ? TRUE : FALSE;
}

static gchar*
get_default_dir(void)
{
	gchar 			*dir;
	GeanyProject 	*project 	= geany->app->project;
	GeanyDocument	*doc 		= document_get_current();

	if (doc != NULL && doc->file_name != NULL && g_path_is_absolute(doc->file_name))
		return utils_get_locale_from_utf8(g_path_get_dirname(doc->file_name));

	if (project)
		dir = project->base_path;
	else
		dir = geany->prefs->default_open_path;

	if (NZV(dir))
		return utils_get_locale_from_utf8(dir);

	return g_get_current_dir();
}

static void
treebrowser_chroot(gchar *directory)
{
	gtk_entry_set_text(GTK_ENTRY(addressbar), directory);

	if (! g_file_test(directory, G_FILE_TEST_IS_DIR))
	{
		dialogs_show_msgbox(GTK_MESSAGE_ERROR, _("Directory '%s' not exists."), directory);
		return;
	}

	gtk_tree_store_clear(treestore);
	addressbar_last_address = directory;
	treebrowser_browse(directory, NULL, CONFIG_INITIAL_DIR_DEEP);
}

static void
treebrowser_browse(gchar *directory, gpointer parent, gint deep_limit)
{
	GtkTreeIter 	iter, *last_dir_iter = NULL;
	gboolean 		is_dir;
	gchar 			*utf8_name;
	GSList 			*list, *node;

	if (deep_limit < 1)
		return;

	deep_limit--;

	directory = g_strconcat(directory, G_DIR_SEPARATOR_S, NULL);

	gtk_tree_store_iter_clear_nodes(parent, FALSE);

	list = utils_get_file_list(directory, NULL, NULL);
	if (list != NULL)
	{
		foreach_slist_free(node, list)
		{
			gchar *fname 	= node->data;
			gchar *uri 		= g_strconcat(directory, fname, NULL);
			is_dir 			= g_file_test (uri, G_FILE_TEST_IS_DIR);
			utf8_name 		= utils_get_utf8_from_locale(fname);

			if (!(fname[0] == '.' && CONFIG_SHOW_HIDDEN_FILES == FALSE))
			{
				if (is_dir)
				{
					if (last_dir_iter == NULL)
						gtk_tree_store_prepend(treestore, &iter, parent);
					else
					{
						gtk_tree_store_insert_after(treestore, &iter, parent, last_dir_iter);
						gtk_tree_iter_free(last_dir_iter);
					}
					last_dir_iter = gtk_tree_iter_copy(&iter);
					gtk_tree_store_set(treestore, &iter,
										TREEBROWSER_COLUMN_ICON, 	GTK_STOCK_DIRECTORY,
										TREEBROWSER_COLUMN_NAME, 	fname,
										TREEBROWSER_COLUMN_URI, 	uri,
										-1);
					if (deep_limit > 0)
						treebrowser_browse(uri, &iter, deep_limit);
				}
				else
				{
					if (check_filtered(utf8_name))
					{
						gtk_tree_store_append(treestore, &iter, parent);
						gtk_tree_store_set(treestore, &iter,
										TREEBROWSER_COLUMN_ICON, 	GTK_STOCK_FILE,
										TREEBROWSER_COLUMN_NAME, 	fname,
										TREEBROWSER_COLUMN_URI, 	uri,
										-1);
					}
				}
			}
			g_free(fname);
			g_free(uri);
		}
	}

}

static gboolean
treebrowser_search(gchar *uri, gpointer parent)
{
	GtkTreeIter 	iter;
	GtkTreePath 	*path;
	gchar 			*uri_current;

	gtk_tree_model_iter_children(GTK_TREE_MODEL(treestore), &iter, parent);
	do
	{
		gtk_tree_model_get(GTK_TREE_MODEL(treestore), &iter, TREEBROWSER_COLUMN_URI, &uri_current, -1);

		if (gtk_tree_model_iter_has_child(GTK_TREE_MODEL(treestore), &iter))
		{
			if (treebrowser_search(uri, &iter))
				return TRUE;
		}

		if (utils_str_equal(uri,uri_current) == TRUE)
		{
			path = gtk_tree_model_get_path(GTK_TREE_MODEL(treestore), &iter);
			gtk_tree_view_expand_to_path(GTK_TREE_VIEW(treeview), path);
			gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(treeview), path, TREEBROWSER_COLUMN_ICON, FALSE, 0, 0);
			gtk_tree_view_set_cursor(GTK_TREE_VIEW(treeview), path, treeview_column_text, FALSE);
			return TRUE;
		}

	} while(gtk_tree_model_iter_next(GTK_TREE_MODEL(treestore), &iter));

	return FALSE;
}

static void
fs_remove(gchar *root, gboolean delete_root)
{

	if (! g_file_test(root, G_FILE_TEST_EXISTS))
		return;

	if (g_file_test(root, G_FILE_TEST_IS_DIR))
	{

		GDir *dir;
		const gchar *name;

		dir = g_dir_open (root, 0, NULL);

		if (!dir)
			return;

		name = g_dir_read_name (dir);
		while (name != NULL)
		{
			gchar *path;
			path = g_build_filename (root, name, NULL);
			if (g_file_test (path, G_FILE_TEST_IS_DIR))
				fs_remove(path, delete_root);
			g_remove(path);
			name = g_dir_read_name(dir);
			g_free (path);
		}
	}
	else
		delete_root = TRUE;

	if (delete_root)
		g_remove(root);

	return;
}

static void
showbars(gboolean state)
{
	if (state) 	gtk_widget_show(sidebar_vbox_bars);
	else 		gtk_widget_hide(sidebar_vbox_bars);
	CONFIG_SHOW_BARS = state;
	gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menu_showbars), CONFIG_SHOW_BARS);
}

static void
gtk_tree_store_iter_clear_nodes(gpointer iter, gboolean delete_root)
{
	GtkTreeIter 	i;
	while (gtk_tree_model_iter_children(GTK_TREE_MODEL(treestore), &i, iter))
	{
		if (gtk_tree_model_iter_has_child(GTK_TREE_MODEL(treestore), &i))
			gtk_tree_store_iter_clear_nodes(&i, TRUE);
		gtk_tree_store_remove(GTK_TREE_STORE(treestore), &i);
	}

	if (delete_root)
		gtk_tree_store_remove(GTK_TREE_STORE(treestore), iter);
}

static gboolean
treebrowser_track_current(void)
{

	GeanyDocument	*doc 		= document_get_current();
	gchar 			*path_current, *path_search = G_DIR_SEPARATOR_S;
	gchar			**path_segments;
	gint 			i;

	if (doc != NULL && doc->file_name != NULL && g_path_is_absolute(doc->file_name))
	{
		path_current = utils_get_locale_from_utf8(doc->file_name);

		path_segments = g_strsplit(path_current, G_DIR_SEPARATOR_S, 0);

		treebrowser_search(path_current, NULL);

		return FALSE;
		for (i = 0; path_segments[i]; i++)
		{
			path_search = g_build_filename(path_search, path_segments[i], NULL);
			/* dialogs_show_msgbox(GTK_MESSAGE_INFO, "%s", path_search);
			 */
			treebrowser_search(path_search, NULL);
			return FALSE;
		}

		return TRUE;
	}
	return FALSE;
}


/* ------------------
 * RIGHTCLICK MENU EVENTS
 * ------------------*/

static void
on_menu_go_up(GtkMenuItem *menuitem, gpointer *user_data)
{
	treebrowser_chroot(g_path_get_dirname(addressbar_last_address));
}

static void
on_menu_current_path(GtkMenuItem *menuitem, gpointer *user_data)
{
	treebrowser_chroot(get_default_dir());
}

static void
on_menu_open_externally(GtkMenuItem *menuitem, gpointer *user_data)
{

	GtkTreeSelection 	*selection 	= gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
	GtkTreeIter 		iter;
	GtkTreeModel 		*model;
	gchar 				*uri;

	gchar 				*cmd, *locale_cmd, *dir;
	GString 			*cmd_str 	= g_string_new(CONFIG_OPEN_EXTERNAL_CMD);
	GError 				*error 		= NULL;

	if (gtk_tree_selection_get_selected(selection, &model, &iter))
	{
		gtk_tree_model_get(model, &iter, TREEBROWSER_COLUMN_URI, &uri, -1);

		dir = g_file_test(uri, G_FILE_TEST_IS_DIR) ? g_strdup(uri) : g_path_get_dirname(uri);

		utils_string_replace_all(cmd_str, "%f", uri);
		utils_string_replace_all(cmd_str, "%d", dir);

		cmd = g_string_free(cmd_str, FALSE);
		locale_cmd = utils_get_locale_from_utf8(cmd);
		if (! g_spawn_command_line_async(locale_cmd, &error))
		{
			gchar *c = strchr(cmd, ' ');
			if (c != NULL)
				*c = '\0';
			ui_set_statusbar(TRUE,
				_("Could not execute configured external command '%s' (%s)."),
				cmd, error->message);
			g_error_free(error);
		}
		g_free(locale_cmd);
		g_free(cmd);
		g_free(dir);

	}

}

static void
on_menu_set_as_root(GtkMenuItem *menuitem, const gchar *type)
{

	GtkTreeSelection 	*selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
	GtkTreeIter 		iter;
	GtkTreeModel 		*model;
	gchar 				*uri;

	if (gtk_tree_selection_get_selected(selection, &model, &iter))
	{
		gtk_tree_model_get(model, &iter, TREEBROWSER_COLUMN_URI, &uri, -1);
		if (g_file_test(uri, G_FILE_TEST_IS_DIR))
			treebrowser_chroot(uri);
	}
}

static void
on_menu_create_new_object(GtkMenuItem *menuitem, gchar *type)
{
	GtkTreeSelection 	*selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
	GtkTreeIter 		iter;
	GtkTreeModel 		*model;
	gchar 				*uri, *uri_new;
	GtkTreePath 		*path_parent;

	if (gtk_tree_selection_get_selected(selection, &model, &iter))
	{
		gtk_tree_model_get(model, &iter, TREEBROWSER_COLUMN_URI, &uri, -1);
		if (! g_file_test(uri, G_FILE_TEST_IS_DIR))
		{
			uri 		= g_path_get_dirname(uri);
			path_parent = gtk_tree_model_get_path(GTK_TREE_MODEL(treestore), &iter);
			if (gtk_tree_path_up(path_parent))
				gtk_tree_model_get_iter(GTK_TREE_MODEL(treestore), &iter, path_parent);
		}
	}
	else return;

	if (utils_str_equal(type, "directory"))
		uri_new = g_strconcat(uri, G_DIR_SEPARATOR_S, _("NewDirectory"), NULL);
	else
		if (utils_str_equal(type, "file"))
			uri_new = g_strconcat(uri, G_DIR_SEPARATOR_S, _("NewFile"), NULL);
		else
			return;

	while(g_file_test(uri_new, G_FILE_TEST_EXISTS))
		uri_new = g_strconcat(uri_new, "_", NULL);

	if (utils_str_equal(type, "directory"))
	{
		if (g_mkdir(uri_new, 0755) == 0)
			treebrowser_browse(uri, &iter, CONFIG_INITIAL_DIR_DEEP);
	}
	else
	{
		if (g_creat(uri_new, 0755) != -1)
			treebrowser_browse(uri, &iter, CONFIG_INITIAL_DIR_DEEP);
	}
}

static void
on_menu_rename(GtkMenuItem *menuitem, gpointer *user_data)
{
	GtkTreeSelection 	*selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
	GtkTreeIter 		iter;
	GtkTreeModel 		*model;
	GtkTreeViewColumn 	*column;
	GtkCellRenderer 	*renderer;
	GtkTreePath 		*path;
	GList 				*renderers;

	if (gtk_tree_selection_get_selected(selection, &model, &iter))
	{
		path = gtk_tree_model_get_path(GTK_TREE_MODEL(treestore), &iter);
		if (G_LIKELY(path != NULL))
		{
			column 		= gtk_tree_view_get_column(GTK_TREE_VIEW(treeview), 0);

			renderers 	= _gtk_cell_layout_get_cells(column);

			renderer 	= g_list_nth_data(renderers, TREEBROWSER_RENDER_TEXT);

			g_object_set(G_OBJECT(renderer), "editable", TRUE, NULL);
			gtk_tree_view_set_cursor_on_cell(GTK_TREE_VIEW(treeview), path, column, renderer, TRUE);

			gtk_tree_path_free(path);
			g_list_free(renderers);
		}
	}
}

static void
on_menu_delete(GtkMenuItem *menuitem, gpointer *user_data)
{

	GtkTreeSelection 	*selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
	GtkTreeIter 		iter;
	GtkTreeModel 		*model;
	GtkTreePath 		*path_parent;
	gchar 				*uri;

	if (gtk_tree_selection_get_selected(selection, &model, &iter))
	{
		gtk_tree_model_get(model, &iter, TREEBROWSER_COLUMN_URI, &uri, -1);

		if (dialogs_show_question(_("Do you really want to delete '%s' ?"), uri))
		{
			fs_remove(uri, TRUE);
			path_parent = gtk_tree_model_get_path(GTK_TREE_MODEL(treestore), &iter);
			if (gtk_tree_path_up(path_parent))
				gtk_tree_model_get_iter(GTK_TREE_MODEL(treestore), &iter, path_parent);
			treebrowser_browse(g_path_get_dirname(uri), &iter, CONFIG_INITIAL_DIR_DEEP);
		}
	}
}

static void
on_menu_refresh(GtkMenuItem *menuitem, gpointer *user_data)
{
	GtkTreeSelection 	*selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
	GtkTreeIter 		iter;
	GtkTreeModel 		*model;
	gchar 				*uri;
	gboolean 			expanded = FALSE;

	if (gtk_tree_selection_get_selected(selection, &model, &iter))
	{
		gtk_tree_model_get(model, &iter, TREEBROWSER_COLUMN_URI, &uri, -1);
		if (g_file_test(uri, G_FILE_TEST_IS_DIR))
		{
			if (gtk_tree_view_row_expanded(GTK_TREE_VIEW(treeview), gtk_tree_model_get_path(GTK_TREE_MODEL(treestore), &iter)))
				expanded = TRUE;

			treebrowser_browse(uri, &iter, CONFIG_INITIAL_DIR_DEEP);

			if (expanded)
				gtk_tree_view_expand_row(GTK_TREE_VIEW(treeview), gtk_tree_model_get_path(GTK_TREE_MODEL(treestore), &iter), FALSE);
		}
	}
}

static void
on_menu_expand_all(GtkMenuItem *menuitem, gpointer *user_data)
{
	gtk_tree_view_expand_all(GTK_TREE_VIEW(treeview));
}

static void
on_menu_collapse_all(GtkMenuItem *menuitem, gpointer *user_data)
{
	gtk_tree_view_collapse_all(GTK_TREE_VIEW(treeview));
}

static void
on_menu_show_bars(GtkMenuItem *menuitem, gpointer *user_data)
{
	showbars(gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(menuitem)));
}

static GtkWidget*
create_popup_menu(gpointer *user_data)
{
	GtkWidget *item, *menu;

	menu = gtk_menu_new();

	item = ui_image_menu_item_new(GTK_STOCK_GO_UP, _("Go up"));
	gtk_container_add(GTK_CONTAINER(menu), item);
	g_signal_connect(item, "activate", G_CALLBACK(on_menu_go_up), NULL);

	item = ui_image_menu_item_new(GTK_STOCK_GO_UP, _("Set path from document"));
	gtk_container_add(GTK_CONTAINER(menu), item);
	g_signal_connect(item, "activate", G_CALLBACK(on_menu_current_path), NULL);

	item = ui_image_menu_item_new(GTK_STOCK_OPEN, _("Open externally"));
	gtk_container_add(GTK_CONTAINER(menu), item);
	g_signal_connect(item, "activate", G_CALLBACK(on_menu_open_externally), NULL);

	item = ui_image_menu_item_new(GTK_STOCK_OPEN, _("Set as root"));
	gtk_container_add(GTK_CONTAINER(menu), item);
	g_signal_connect(item, "activate", G_CALLBACK(on_menu_set_as_root), NULL);

	item = gtk_separator_menu_item_new();
	gtk_widget_show(item);
	gtk_container_add(GTK_CONTAINER(menu), item);

	item = ui_image_menu_item_new(GTK_STOCK_ADD, _("Create new directory"));
	gtk_container_add(GTK_CONTAINER(menu), item);
	g_signal_connect(item, "activate", G_CALLBACK(on_menu_create_new_object), "directory");

	item = ui_image_menu_item_new(GTK_STOCK_NEW, _("Create new file"));
	gtk_container_add(GTK_CONTAINER(menu), item);
	g_signal_connect(item, "activate", G_CALLBACK(on_menu_create_new_object), "file");

	item = ui_image_menu_item_new(GTK_STOCK_SAVE_AS, _("Rename"));
	gtk_container_add(GTK_CONTAINER(menu), item);
	g_signal_connect(item, "activate", G_CALLBACK(on_menu_rename), NULL);

	item = ui_image_menu_item_new(GTK_STOCK_DELETE, _("Delete"));
	gtk_container_add(GTK_CONTAINER(menu), item);
	g_signal_connect(item, "activate", G_CALLBACK(on_menu_delete), NULL);

	item = gtk_separator_menu_item_new();
	gtk_widget_show(item);
	gtk_container_add(GTK_CONTAINER(menu), item);

	item = ui_image_menu_item_new(GTK_STOCK_REFRESH, _("Refresh"));
	gtk_container_add(GTK_CONTAINER(menu), item);
	g_signal_connect(item, "activate", G_CALLBACK(on_menu_refresh), NULL);

	item = gtk_separator_menu_item_new();
	gtk_widget_show(item);
	gtk_container_add(GTK_CONTAINER(menu), item);

	item = ui_image_menu_item_new(GTK_STOCK_GO_FORWARD, _("Expand all"));
	gtk_container_add(GTK_CONTAINER(menu), item);
	g_signal_connect(item, "activate", G_CALLBACK(on_menu_expand_all), NULL);

	item = ui_image_menu_item_new(GTK_STOCK_GO_BACK, _("Collapse all"));
	gtk_container_add(GTK_CONTAINER(menu), item);
	g_signal_connect(item, "activate", G_CALLBACK(on_menu_collapse_all), NULL);

	item = gtk_separator_menu_item_new();
	gtk_widget_show(item);
	gtk_container_add(GTK_CONTAINER(menu), item);

	menu_showbars = gtk_check_menu_item_new_with_mnemonic(_("Show bars"));
	gtk_container_add(GTK_CONTAINER(menu), menu_showbars);
	gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menu_showbars), CONFIG_SHOW_BARS);
	g_signal_connect(menu_showbars, "activate", G_CALLBACK(on_menu_show_bars), NULL);

	gtk_widget_show_all(menu);

	return menu;
}


/* ------------------
 * TOOLBAR`S EVENTS
 * ------------------ */

static void
on_button_go_up(void)
{
	treebrowser_chroot(g_path_get_dirname(addressbar_last_address));
}

static void
on_button_refresh(void)
{
	treebrowser_chroot(addressbar_last_address);
}

static void
on_button_go_home(void)
{
	treebrowser_chroot(g_strdup(g_get_home_dir()));
}

static void
on_button_current_path(void)
{
	treebrowser_chroot(get_default_dir());
}

static void
on_button_hide_bars(void)
{
	showbars(FALSE);
}

static void
on_addressbar_activate(GtkEntry *entry, gpointer user_data)
{
	gchar *directory = gtk_editable_get_chars(GTK_EDITABLE(entry), 0, -1);
	treebrowser_chroot(directory);
}

static void
on_filter_activate(GtkEntry *entry, gpointer user_data)
{
	treebrowser_chroot(addressbar_last_address);
}


/* ------------------
 * TREEVIEW EVENTS
 * ------------------ */

static gboolean
on_treeview_mouseclick(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
	if (event->button == 3)
	{
		static GtkWidget *popup_menu = NULL;
		if (popup_menu == NULL)
			popup_menu = create_popup_menu(user_data);
		gtk_menu_popup(GTK_MENU(popup_menu), NULL, NULL, NULL, NULL, event->button, event->time);
	}
	return FALSE;
}

static void
on_treeview_changed(GtkWidget *widget, gpointer user_data)
{
	GtkTreeIter 	iter;
	GtkTreeModel 	*model;
	gchar 			*uri;
	gboolean 		expanded = FALSE;

	if (gtk_tree_selection_get_selected(GTK_TREE_SELECTION(widget), &model, &iter))
	{
		gtk_tree_model_get(GTK_TREE_MODEL(treestore), &iter,
							TREEBROWSER_COLUMN_URI, &uri,
							-1);
		if (g_file_test(uri, G_FILE_TEST_EXISTS))
		{
			if (g_file_test(uri, G_FILE_TEST_IS_DIR))
			{
				if (gtk_tree_view_row_expanded(GTK_TREE_VIEW(treeview), gtk_tree_model_get_path(GTK_TREE_MODEL(treestore), &iter))
					|| !gtk_tree_model_iter_has_child(GTK_TREE_MODEL(treestore), &iter))
					expanded = TRUE;

				treebrowser_browse(uri, &iter, CONFIG_INITIAL_DIR_DEEP);

				if (expanded)
					gtk_tree_view_expand_row(GTK_TREE_VIEW(treeview), gtk_tree_model_get_path(GTK_TREE_MODEL(treestore), &iter), FALSE);
			}
			else
				if (CONFIG_ONE_CLICK_CHDOC)
					document_open_file(uri, FALSE, NULL, NULL);
		}
		else
			gtk_tree_store_iter_clear_nodes(&iter, TRUE);
	}
}

static void
on_treeview_row_activated(GtkWidget *widget, GtkTreePath *path, GtkTreeViewColumn *column, gpointer user_data)
{
	GtkTreeIter 	iter;
	gchar 			*uri;

	gtk_tree_model_get_iter(GTK_TREE_MODEL(treestore), &iter, path);
	gtk_tree_model_get(GTK_TREE_MODEL(treestore), &iter,
							TREEBROWSER_COLUMN_URI,  &uri,
							-1);

	if (g_file_test (uri, G_FILE_TEST_IS_DIR))
		if (CONFIG_CHROOT_ON_DCLICK)
			treebrowser_chroot(uri);
		else
			if (gtk_tree_view_row_expanded(GTK_TREE_VIEW(widget), path))
				gtk_tree_view_collapse_row(GTK_TREE_VIEW(widget), path);
			else
				gtk_tree_view_expand_row(GTK_TREE_VIEW(widget), path, FALSE);
	else
		document_open_file(uri, FALSE, NULL, NULL);
}

static void
on_treeview_row_expanded(GtkWidget *widget, GtkTreeIter *iter, GtkTreePath *path, gpointer user_data)
{
	gtk_tree_store_set(treestore, iter, TREEBROWSER_COLUMN_ICON, GTK_STOCK_OPEN, -1);
}

static void
on_treeview_row_collapsed(GtkWidget *widget, GtkTreeIter *iter, GtkTreePath *path, gpointer user_data)
{
	gtk_tree_store_set(treestore, iter, TREEBROWSER_COLUMN_ICON, GTK_STOCK_DIRECTORY, -1);
}

static void
on_treeview_renamed(GtkCellRenderer *renderer, const gchar *path_string, const gchar *name_new, gpointer data)
{

	GtkTreeViewColumn 	*column;
	GList 				*renderers;
	GtkTreeIter 		iter, iter_parent;
	gchar 				*uri, *uri_new;
	GtkTreePath 		*path_parent;

	column 		= gtk_tree_view_get_column(GTK_TREE_VIEW(treeview), 0);
	renderers 	= _gtk_cell_layout_get_cells(column);
	renderer 	= g_list_nth_data(renderers, TREEBROWSER_RENDER_TEXT);

	g_object_set(G_OBJECT(renderer), "editable", FALSE, NULL);

	if (gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(treestore), &iter, path_string))
	{
		gtk_tree_model_get(GTK_TREE_MODEL(treestore), &iter, TREEBROWSER_COLUMN_URI, &uri, -1);
		if (uri)
		{
			uri_new = g_strconcat(g_path_get_dirname(uri), G_DIR_SEPARATOR_S, name_new, NULL);
			if (g_rename(uri, uri_new) == 0)
			{
				gtk_tree_store_set(treestore, &iter,
								TREEBROWSER_COLUMN_NAME, name_new,
								TREEBROWSER_COLUMN_URI, uri_new,
								-1);

				path_parent = gtk_tree_model_get_path(GTK_TREE_MODEL(treestore), &iter);

				if (gtk_tree_path_up(path_parent))
				{
					if (gtk_tree_model_get_iter(GTK_TREE_MODEL(treestore), &iter_parent, path_parent))
						treebrowser_browse(g_path_get_dirname(uri_new), &iter_parent, CONFIG_INITIAL_DIR_DEEP);
					else
						treebrowser_browse(g_path_get_dirname(uri_new), NULL, CONFIG_INITIAL_DIR_DEEP);
				}
				else
					treebrowser_browse(g_path_get_dirname(uri_new), NULL, CONFIG_INITIAL_DIR_DEEP);
			}
		}
	}
}

static void
treebrowser_track_current_cb(void)
{
	if (CONFIG_FOLLOW_CURRENT_DOC)
		treebrowser_track_current();
}


/* ------------------
 * TREEBROWSER INITIAL FUNCTIONS
 * ------------------ */

static void
create_sidebar(void)
{
	GtkWidget 			*scrollwin;
	GtkWidget 			*toolbar;
	GtkWidget 			*wid;
	GtkTreeSelection 	*selection;

	sidebar_vbox 		= gtk_vbox_new(FALSE, 0);
	sidebar_vbox_bars 	= gtk_vbox_new(FALSE, 0);

	selection 			= gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
	addressbar 			= gtk_entry_new();
	filter 				= gtk_entry_new();

	scrollwin 			= gtk_scrolled_window_new(NULL, NULL);

	page_number 		= gtk_notebook_append_page(GTK_NOTEBOOK(geany->main_widgets->sidebar_notebook),
							sidebar_vbox, gtk_label_new(_("Tree Browser")));

	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrollwin), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

	toolbar = gtk_toolbar_new();
	gtk_toolbar_set_icon_size(GTK_TOOLBAR(toolbar), GTK_ICON_SIZE_MENU);
	gtk_toolbar_set_style(GTK_TOOLBAR(toolbar), GTK_TOOLBAR_ICONS);

	wid = GTK_WIDGET(gtk_tool_button_new_from_stock(GTK_STOCK_GO_UP));
	ui_widget_set_tooltip_text(wid, _("Go up"));
	g_signal_connect(wid, "clicked", G_CALLBACK(on_button_go_up), NULL);
	gtk_container_add(GTK_CONTAINER(toolbar), wid);

	wid = GTK_WIDGET(gtk_tool_button_new_from_stock(GTK_STOCK_REFRESH));
	ui_widget_set_tooltip_text(wid, _("Refresh"));
	g_signal_connect(wid, "clicked", G_CALLBACK(on_button_refresh), NULL);
	gtk_container_add(GTK_CONTAINER(toolbar), wid);

	wid = GTK_WIDGET(gtk_tool_button_new_from_stock(GTK_STOCK_HOME));
	ui_widget_set_tooltip_text(wid, _("Home"));
	g_signal_connect(wid, "clicked", G_CALLBACK(on_button_go_home), NULL);
	gtk_container_add(GTK_CONTAINER(toolbar), wid);

	wid = GTK_WIDGET(gtk_tool_button_new_from_stock(GTK_STOCK_JUMP_TO));
	ui_widget_set_tooltip_text(wid, _("Set path from document"));
	g_signal_connect(wid, "clicked", G_CALLBACK(on_button_current_path), NULL);
	gtk_container_add(GTK_CONTAINER(toolbar), wid);

	wid = GTK_WIDGET(gtk_tool_button_new_from_stock(GTK_STOCK_DIRECTORY));
	ui_widget_set_tooltip_text(wid, _("Track path"));
	g_signal_connect(wid, "clicked", G_CALLBACK(treebrowser_track_current), NULL);
	gtk_container_add(GTK_CONTAINER(toolbar), wid);

	wid = GTK_WIDGET(gtk_tool_button_new_from_stock(GTK_STOCK_CLOSE));
	ui_widget_set_tooltip_text(wid, _("Hide bars"));
	g_signal_connect(wid, "clicked", G_CALLBACK(on_button_hide_bars), NULL);
	gtk_container_add(GTK_CONTAINER(toolbar), wid);

	gtk_container_add(GTK_CONTAINER(scrollwin), 	treeview);
	gtk_box_pack_start(GTK_BOX(sidebar_vbox_bars), 			filter, 			FALSE, TRUE,  1);
	gtk_box_pack_start(GTK_BOX(sidebar_vbox_bars), 			addressbar, 		FALSE, TRUE,  1);
	gtk_box_pack_start(GTK_BOX(sidebar_vbox_bars), 			toolbar, 			FALSE, TRUE,  1);

	gtk_box_pack_start(GTK_BOX(sidebar_vbox), 				scrollwin, 			TRUE,  TRUE,  1);
	gtk_box_pack_start(GTK_BOX(sidebar_vbox), 				sidebar_vbox_bars, 	FALSE, TRUE,  1);

	g_signal_connect(selection, 		"changed", 				G_CALLBACK(on_treeview_changed), 				NULL);
	g_signal_connect(treeview, 			"button-press-event", 	G_CALLBACK(on_treeview_mouseclick), 			selection);
	g_signal_connect(treeview, 			"row-activated", 		G_CALLBACK(on_treeview_row_activated), 			NULL);
	g_signal_connect(treeview, 			"row-collapsed", 		G_CALLBACK(on_treeview_row_collapsed), 			NULL);
	g_signal_connect(treeview, 			"row-expanded", 		G_CALLBACK(on_treeview_row_expanded), 			NULL);
	g_signal_connect(addressbar, 		"activate", 			G_CALLBACK(on_addressbar_activate), 			NULL);
	g_signal_connect(filter, 			"activate", 			G_CALLBACK(on_filter_activate), 				NULL);

	gtk_widget_show_all(sidebar_vbox);

	showbars(CONFIG_SHOW_BARS);
}

static GtkWidget*
create_view_and_model(void)
{

	GtkWidget 			*view;

	view 					= gtk_tree_view_new();
	treeview_column_icon	= gtk_tree_view_column_new();
	treeview_column_text	= gtk_tree_view_column_new();
	render_icon 			= gtk_cell_renderer_pixbuf_new();
	render_text 			= gtk_cell_renderer_text_new();

	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(view), FALSE);

	gtk_tree_view_append_column(GTK_TREE_VIEW(view), treeview_column_text);

	gtk_tree_view_column_pack_start(treeview_column_text, render_icon, FALSE);
	gtk_tree_view_column_set_attributes(treeview_column_text, render_icon, "stock-id", TREEBROWSER_RENDER_ICON, NULL);

	gtk_tree_view_column_pack_start(treeview_column_text, render_text, TRUE);
	gtk_tree_view_column_add_attribute(treeview_column_text, render_text, "text", TREEBROWSER_RENDER_TEXT);

	treestore = gtk_tree_store_new(TREEBROWSER_COLUMNC, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);

	gtk_tree_view_set_model(GTK_TREE_VIEW(view), GTK_TREE_MODEL(treestore));

	g_signal_connect(G_OBJECT(render_text), "edited", G_CALLBACK(on_treeview_renamed), view);

	return view;
}


/* ------------------
 * CONFIG DIALOG
 * ------------------ */

static struct
{
	GtkWidget *OPEN_EXTERNAL_CMD;
	GtkWidget *INITIAL_DIR_DEEP;
	GtkWidget *REVERSE_FILTER;
	GtkWidget *ONE_CLICK_CHDOC;
	GtkWidget *SHOW_HIDDEN_FILES;
	GtkWidget *SHOW_BARS;
	GtkWidget *CHROOT_ON_DCLICK;
	GtkWidget *FOLLOW_CURRENT_DOC;
} configure_widgets;

static void
load_settings(void)
{
	GKeyFile *config 	= g_key_file_new();

	g_key_file_load_from_file(config, CONFIG_FILE, G_KEY_FILE_NONE, NULL);

	CONFIG_OPEN_EXTERNAL_CMD       = utils_get_setting_string(config, "treebrowser", "open_external_cmd", CONFIG_OPEN_EXTERNAL_CMD);
	CONFIG_INITIAL_DIR_DEEP        = utils_get_setting_integer(config, "treebrowser", "initial_dir_deep", CONFIG_INITIAL_DIR_DEEP);
	CONFIG_REVERSE_FILTER   = utils_get_setting_boolean(config, "treebrowser", "reverse_filter", CONFIG_REVERSE_FILTER);
	CONFIG_ONE_CLICK_CHDOC                 = utils_get_setting_boolean(config, "treebrowser", "one_click_chdoc", CONFIG_ONE_CLICK_CHDOC);
	CONFIG_SHOW_HIDDEN_FILES       = utils_get_setting_boolean(config, "treebrowser", "show_hidden_files", CONFIG_SHOW_HIDDEN_FILES);
	CONFIG_SHOW_BARS                   = utils_get_setting_boolean(config, "treebrowser", "show_bars", CONFIG_SHOW_BARS);
	CONFIG_CHROOT_ON_DCLICK                = utils_get_setting_boolean(config, "treebrowser", "chroot_on_dclick", CONFIG_CHROOT_ON_DCLICK);
	CONFIG_FOLLOW_CURRENT_DOC      = utils_get_setting_boolean(config, "treebrowser", "follow_current_doc", CONFIG_FOLLOW_CURRENT_DOC);

	g_key_file_free(config);
}

static void
on_configure_response(GtkDialog *dialog, gint response, gpointer user_data)
{
	GKeyFile 	*config 		= g_key_file_new();
	gchar 		*config_dir 	= g_path_get_dirname(CONFIG_FILE);
	gchar 		*data;

	if (! (response == GTK_RESPONSE_OK || response == GTK_RESPONSE_APPLY))
		return;

	CONFIG_OPEN_EXTERNAL_CMD 	= gtk_editable_get_chars(GTK_EDITABLE(configure_widgets.OPEN_EXTERNAL_CMD), 0, -1);
	CONFIG_INITIAL_DIR_DEEP 	= gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(configure_widgets.INITIAL_DIR_DEEP));
	CONFIG_REVERSE_FILTER 		= gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(configure_widgets.REVERSE_FILTER));
	CONFIG_ONE_CLICK_CHDOC 		= gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(configure_widgets.ONE_CLICK_CHDOC));
	CONFIG_SHOW_HIDDEN_FILES 	= gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(configure_widgets.SHOW_HIDDEN_FILES));
	CONFIG_SHOW_BARS 			= gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(configure_widgets.SHOW_BARS));
	CONFIG_CHROOT_ON_DCLICK 	= gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(configure_widgets.CHROOT_ON_DCLICK));
	CONFIG_FOLLOW_CURRENT_DOC 	= gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(configure_widgets.FOLLOW_CURRENT_DOC));

	g_key_file_load_from_file(config, CONFIG_FILE, G_KEY_FILE_NONE, NULL);
	if (! g_file_test(config_dir, G_FILE_TEST_IS_DIR) && utils_mkdir(config_dir, TRUE) != 0)
		dialogs_show_msgbox(GTK_MESSAGE_ERROR,
			_("Plugin configuration directory could not be created."));
	else
	{
		g_key_file_set_string(config, 	"treebrowser", "open_external_cmd", 	CONFIG_OPEN_EXTERNAL_CMD);
		g_key_file_set_integer(config, 	"treebrowser", "initial_dir_deep", 		CONFIG_INITIAL_DIR_DEEP);
		g_key_file_set_boolean(config, 	"treebrowser", "reverse_filter", 		CONFIG_REVERSE_FILTER);
		g_key_file_set_boolean(config, 	"treebrowser", "one_click_chdoc", 		CONFIG_ONE_CLICK_CHDOC);
		g_key_file_set_boolean(config, 	"treebrowser", "show_hidden_files", 	CONFIG_SHOW_HIDDEN_FILES);
		g_key_file_set_boolean(config, 	"treebrowser", "show_bars", 			CONFIG_SHOW_BARS);
		g_key_file_set_boolean(config, 	"treebrowser", "chroot_on_dclick", 		CONFIG_CHROOT_ON_DCLICK);
		g_key_file_set_boolean(config, 	"treebrowser", "follow_current_doc", 	CONFIG_FOLLOW_CURRENT_DOC);

		/* write config to file */
		data = g_key_file_to_data(config, NULL, NULL);
		utils_write_file(CONFIG_FILE, data);
		g_free(data);

		treebrowser_chroot(addressbar_last_address);
	}

}

GtkWidget*
plugin_configure(GtkDialog *dialog)
{
	GtkWidget *label;
	GtkWidget *vbox, *hbox;

	vbox 	= gtk_vbox_new(FALSE, 0);

	hbox 	= gtk_hbox_new(FALSE, 0);
	label 	= gtk_label_new(_("External open command: "));
	configure_widgets.OPEN_EXTERNAL_CMD = gtk_entry_new();
	gtk_entry_set_text(GTK_ENTRY(configure_widgets.OPEN_EXTERNAL_CMD), CONFIG_OPEN_EXTERNAL_CMD);
	gtk_misc_set_alignment(GTK_MISC(label), 0, 0.5);
	ui_widget_set_tooltip_text(configure_widgets.OPEN_EXTERNAL_CMD,
		_("The command to execute when using \"Open with\". You can use %f and %d wildcards.\n"
		  "%f will be replaced with the filename including full path\n"
		  "%d will be replaced with the path name of the selected file without the filename"));
	gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(hbox), configure_widgets.OPEN_EXTERNAL_CMD, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 6);

	hbox 	= gtk_hbox_new(FALSE, 0);
	label 	= gtk_label_new(_("Default directory deep to fill: "));
	configure_widgets.INITIAL_DIR_DEEP = gtk_spin_button_new_with_range(1, 99, 1);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(configure_widgets.INITIAL_DIR_DEEP), CONFIG_INITIAL_DIR_DEEP);
	gtk_misc_set_alignment(GTK_MISC(label), 0, 0.5);
	gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(hbox), configure_widgets.INITIAL_DIR_DEEP, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 6);

	configure_widgets.SHOW_HIDDEN_FILES = gtk_check_button_new_with_label(_("Show hidden files"));
	gtk_button_set_focus_on_click(GTK_BUTTON(configure_widgets.SHOW_HIDDEN_FILES), FALSE);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(configure_widgets.SHOW_HIDDEN_FILES), CONFIG_SHOW_HIDDEN_FILES);
	gtk_box_pack_start(GTK_BOX(vbox), configure_widgets.SHOW_HIDDEN_FILES, FALSE, FALSE, 0);

	configure_widgets.REVERSE_FILTER = gtk_check_button_new_with_label(_("Reverse filter"));
	gtk_button_set_focus_on_click(GTK_BUTTON(configure_widgets.REVERSE_FILTER), FALSE);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(configure_widgets.REVERSE_FILTER), CONFIG_REVERSE_FILTER);
	gtk_box_pack_start(GTK_BOX(vbox), configure_widgets.REVERSE_FILTER, FALSE, FALSE, 0);

	configure_widgets.FOLLOW_CURRENT_DOC = gtk_check_button_new_with_label(_("Follow current document"));
	gtk_button_set_focus_on_click(GTK_BUTTON(configure_widgets.FOLLOW_CURRENT_DOC), FALSE);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(configure_widgets.FOLLOW_CURRENT_DOC), CONFIG_FOLLOW_CURRENT_DOC);
	gtk_box_pack_start(GTK_BOX(vbox), configure_widgets.FOLLOW_CURRENT_DOC, FALSE, FALSE, 0);

	configure_widgets.ONE_CLICK_CHDOC = gtk_check_button_new_with_label(_("On click document change"));
	gtk_button_set_focus_on_click(GTK_BUTTON(configure_widgets.ONE_CLICK_CHDOC), FALSE);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(configure_widgets.ONE_CLICK_CHDOC), CONFIG_ONE_CLICK_CHDOC);
	gtk_box_pack_start(GTK_BOX(vbox), configure_widgets.ONE_CLICK_CHDOC, FALSE, FALSE, 0);

	configure_widgets.CHROOT_ON_DCLICK = gtk_check_button_new_with_label(_("Chroot on dclick"));
	gtk_button_set_focus_on_click(GTK_BUTTON(configure_widgets.CHROOT_ON_DCLICK), FALSE);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(configure_widgets.CHROOT_ON_DCLICK), CONFIG_CHROOT_ON_DCLICK);
	gtk_box_pack_start(GTK_BOX(vbox), configure_widgets.CHROOT_ON_DCLICK, FALSE, FALSE, 0);

	gtk_widget_show_all(vbox);

	g_signal_connect(dialog, "response", G_CALLBACK(on_configure_response), NULL);

	return vbox;
}


/* ------------------
 * GEANY HOOKS
 * ------------------ */

void
plugin_init(GeanyData *data)
{
	CONFIG_FILE = g_strconcat(geany->app->configdir, G_DIR_SEPARATOR_S, "plugins", G_DIR_SEPARATOR_S,
		"treebrowser", G_DIR_SEPARATOR_S, "treebrowser.conf", NULL);

	load_settings();

	treeview = create_view_and_model();
	create_sidebar();
	treebrowser_chroot(get_default_dir());

	plugin_signal_connect(geany_plugin, NULL, "document-activate", TRUE,
		(GCallback)&treebrowser_track_current_cb, NULL);
}

void
plugin_cleanup(void)
{
	gtk_widget_destroy(GTK_WIDGET(sidebar_vbox));
}