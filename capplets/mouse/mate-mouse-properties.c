/* -*- mode: c; style: linux -*- */

/* mouse-properties-capplet.c
 * Copyright (C) 2001 Red Hat, Inc.
 * Copyright (C) 2001 Ximian, Inc.
 *
 * Written by: Jonathon Blandford <jrb@redhat.com>,
 *             Bradford Hovinen <hovinen@ximian.com>,
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include <config.h>

#include <glib/gi18n.h>
#include <string.h>
#include <gio/gio.h>
#include <gdk/gdkx.h>
#include <math.h>

#include "capplet-util.h"
#include "activate-settings-daemon.h"
#include "capplet-stock-icons.h"
//#include "mate-mouse-accessibility.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#ifdef HAVE_XINPUT
#include <X11/Xatom.h>
#include <X11/extensions/XInput.h>
#endif

#ifdef HAVE_XCURSOR
#include <X11/Xcursor/Xcursor.h>
#endif

#if GTK_CHECK_VERSION (3, 0, 0)
#define GtkFunction GSourceFunc
#endif

enum
{
	DOUBLE_CLICK_TEST_OFF,
	DOUBLE_CLICK_TEST_MAYBE,
	DOUBLE_CLICK_TEST_ON
};

#define MOUSE_SCHEMA "org.mate.peripherals-mouse"
#define DOUBLE_CLICK_KEY "double-click"

#define TOUCHPAD_SCHEMA "org.mate.peripherals-touchpad"

/* State in testing the double-click speed. Global for a great deal of
 * convenience
 */
static gint double_click_state = DOUBLE_CLICK_TEST_OFF;

static GSettings *mouse_settings = NULL;
static GSettings *touchpad_settings = NULL;

static void
get_default_mouse_info (int *default_numerator, int *default_denominator, int *default_threshold)
{
	int numerator, denominator;
	int threshold;
	int tmp_num, tmp_den, tmp_threshold;

	/* Query X for the default value */
	XGetPointerControl (GDK_DISPLAY_XDISPLAY(gdk_display_get_default()), &numerator, &denominator,
			    &threshold);
	XChangePointerControl (GDK_DISPLAY_XDISPLAY(gdk_display_get_default()), True, True, -1, -1, -1);
	XGetPointerControl (GDK_DISPLAY_XDISPLAY(gdk_display_get_default()), &tmp_num, &tmp_den, &tmp_threshold);
	XChangePointerControl (GDK_DISPLAY_XDISPLAY(gdk_display_get_default()), True, True, numerator, denominator, threshold);

	if (default_numerator)
		*default_numerator = tmp_num;

	if (default_denominator)
		*default_denominator = tmp_den;

	if (default_threshold)
		*default_threshold = tmp_threshold;

}

/* Double Click handling */

struct test_data_t
{
	gint *timeout_id;
	GtkWidget *image;
};

/* Timeout for the double click test */

static gboolean
test_maybe_timeout (struct test_data_t *data)
{
	double_click_state = DOUBLE_CLICK_TEST_OFF;

	gtk_image_set_from_stock (GTK_IMAGE (data->image),
				  MOUSE_DBLCLCK_OFF, mouse_capplet_dblclck_icon_get_size());

	*data->timeout_id = 0;

	return FALSE;
}

/* Callback issued when the user clicks the double click testing area. */

static gboolean
event_box_button_press_event (GtkWidget   *widget,
			      GdkEventButton *event,
			      gpointer user_data)
{
	gint                       double_click_time;
	static struct test_data_t  data;
	static gint                test_on_timeout_id     = 0;
	static gint                test_maybe_timeout_id  = 0;
	static guint32             double_click_timestamp = 0;
	GtkWidget                 *image;

	if (event->type != GDK_BUTTON_PRESS)
		return FALSE;

	image = g_object_get_data (G_OBJECT (widget), "image");

	double_click_time = g_settings_get_int (mouse_settings, DOUBLE_CLICK_KEY);

	if (test_maybe_timeout_id != 0)
		g_source_remove  (test_maybe_timeout_id);
	if (test_on_timeout_id != 0)
		g_source_remove (test_on_timeout_id);

	switch (double_click_state) {
	case DOUBLE_CLICK_TEST_OFF:
		double_click_state = DOUBLE_CLICK_TEST_MAYBE;
		data.image = image;
		data.timeout_id = &test_maybe_timeout_id;
		test_maybe_timeout_id = g_timeout_add (double_click_time, (GtkFunction) test_maybe_timeout, &data);
		break;
	case DOUBLE_CLICK_TEST_MAYBE:
		if (event->time - double_click_timestamp < double_click_time) {
			double_click_state = DOUBLE_CLICK_TEST_ON;
			data.image = image;
			data.timeout_id = &test_on_timeout_id;
			test_on_timeout_id = g_timeout_add (2500, (GtkFunction) test_maybe_timeout, &data);
		}
		break;
	case DOUBLE_CLICK_TEST_ON:
		double_click_state = DOUBLE_CLICK_TEST_OFF;
		break;
	}

	double_click_timestamp = event->time;

	switch (double_click_state) {
	case DOUBLE_CLICK_TEST_ON:
		gtk_image_set_from_stock (GTK_IMAGE (image),
					  MOUSE_DBLCLCK_ON, mouse_capplet_dblclck_icon_get_size());
		break;
	case DOUBLE_CLICK_TEST_MAYBE:
		gtk_image_set_from_stock (GTK_IMAGE (image),
					  MOUSE_DBLCLCK_MAYBE, mouse_capplet_dblclck_icon_get_size());
		break;
	case DOUBLE_CLICK_TEST_OFF:
		gtk_image_set_from_stock (GTK_IMAGE (image),
					  MOUSE_DBLCLCK_OFF, mouse_capplet_dblclck_icon_get_size());
		break;
	}

	return TRUE;
}

static void
orientation_radio_button_release_event (GtkWidget   *widget,
				        GdkEventButton *event)
{
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (widget), TRUE);
}

static void
orientation_radio_button_toggled (GtkToggleButton *togglebutton,
				        GtkBuilder *dialog)
{
	gboolean left_handed;
	left_handed = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (WID ("left_handed_radio")));
	g_settings_set_boolean (mouse_settings, "left-handed", left_handed);
}

static void
scrollmethod_gsettings_changed_event (GSettings *settings,
				gchar *key,
				GtkBuilder *dialog)
{
	int scroll_method = g_settings_get_int (touchpad_settings, "scroll-method");
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (WID ("scroll_disabled_radio")),
				scroll_method == 0);
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (WID ("scroll_edge_radio")),
				scroll_method == 1);
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (WID ("scroll_twofinger_radio")),
				scroll_method == 2);
	gtk_widget_set_sensitive (WID ("horiz_scroll_toggle"),
				 scroll_method != 0);
}

static void
scrollmethod_clicked_event (GtkWidget *widget,
				GtkBuilder *dialog)
{
	GtkToggleButton *disabled = GTK_TOGGLE_BUTTON (WID ("scroll_disabled_radio"));

	gtk_widget_set_sensitive (WID ("horiz_scroll_toggle"),
				  !gtk_toggle_button_get_active (disabled));

	GSList *radio_group;
	int new_scroll_method;
	int old_scroll_method = g_settings_get_int (touchpad_settings, "scroll-method");

	if (!gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON(widget)))
		return;

	radio_group = g_slist_copy (gtk_radio_button_get_group
		(GTK_RADIO_BUTTON (WID ("scroll_disabled_radio"))));
	radio_group = g_slist_reverse (radio_group);
	new_scroll_method = g_slist_index (radio_group, widget);
	g_slist_free (radio_group);
	
	if (new_scroll_method != old_scroll_method)
		g_settings_set_int (touchpad_settings, "scroll-method", new_scroll_method);
}

static void
synaptics_check_capabilities (GtkBuilder *dialog)
{
#ifdef HAVE_XINPUT
	int numdevices, i;
	XDeviceInfo *devicelist;
	Atom realtype, prop;
	int realformat;
	unsigned long nitems, bytes_after;
	unsigned char *data;

	prop = XInternAtom (GDK_DISPLAY_XDISPLAY(gdk_display_get_default()), "Synaptics Capabilities", True);
	if (!prop)
		return;

	devicelist = XListInputDevices (GDK_DISPLAY_XDISPLAY(gdk_display_get_default()), &numdevices);
	for (i = 0; i < numdevices; i++) {
		if (devicelist[i].use != IsXExtensionPointer)
			continue;

		gdk_error_trap_push ();
		XDevice *device = XOpenDevice (GDK_DISPLAY_XDISPLAY(gdk_display_get_default()),
					       devicelist[i].id);
		if (gdk_error_trap_pop ())
			continue;

		gdk_error_trap_push ();
		if ((XGetDeviceProperty (GDK_DISPLAY_XDISPLAY(gdk_display_get_default()), device, prop, 0, 2, False,
					 XA_INTEGER, &realtype, &realformat, &nitems,
					 &bytes_after, &data) == Success) && (realtype != None)) {
			/* Property data is booleans for has_left, has_middle,
			 * has_right, has_double, has_triple */
			if (!data[0]) {
				gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (WID ("tap_to_click_toggle")), TRUE);
				gtk_widget_set_sensitive (WID ("tap_to_click_toggle"), FALSE);
			}

			if (!data[3])
				gtk_widget_set_sensitive (WID ("scroll_twofinger_radio"), FALSE);

			XFree (data);
		}
		gdk_error_trap_pop ();

		XCloseDevice (GDK_DISPLAY_XDISPLAY(gdk_display_get_default()), device);
	}
	XFreeDeviceList (devicelist);
#endif
}

static gboolean
find_synaptics (void)
{
	gboolean ret = FALSE;
#ifdef HAVE_XINPUT
	int numdevices, i;
	XDeviceInfo *devicelist;
	Atom realtype, prop;
	int realformat;
	unsigned long nitems, bytes_after;
	unsigned char *data;
	XExtensionVersion *version;

	/* Input device properties require version 1.5 or higher */
	version = XGetExtensionVersion (GDK_DISPLAY_XDISPLAY(gdk_display_get_default()), "XInputExtension");
	if (!version->present ||
		(version->major_version * 1000 + version->minor_version) < 1005) {
		XFree (version);
		return False;
	}

	prop = XInternAtom (GDK_DISPLAY_XDISPLAY(gdk_display_get_default()), "Synaptics Off", True);
	if (!prop)
		return False;

	devicelist = XListInputDevices (GDK_DISPLAY_XDISPLAY(gdk_display_get_default()), &numdevices);
	for (i = 0; i < numdevices; i++) {
		if (devicelist[i].use != IsXExtensionPointer)
			continue;

		gdk_error_trap_push();
		XDevice *device = XOpenDevice (GDK_DISPLAY_XDISPLAY(gdk_display_get_default()),
					       devicelist[i].id);
		if (gdk_error_trap_pop ())
			continue;

		gdk_error_trap_push ();
		if ((XGetDeviceProperty (GDK_DISPLAY_XDISPLAY(gdk_display_get_default()), device, prop, 0, 1, False,
					 XA_INTEGER, &realtype, &realformat, &nitems,
					 &bytes_after, &data) == Success) && (realtype != None)) {
			XFree (data);
			ret = TRUE;
		}
		gdk_error_trap_pop ();

		XCloseDevice (GDK_DISPLAY_XDISPLAY(gdk_display_get_default()), device);

		if (ret)
			break;
	}

	XFree (version);
	XFreeDeviceList (devicelist);
#endif
	return ret;
}

enum
{
	COLUMN_DEVICE_NAME,
	COLUMN_DEVICE_XID,
	N_DEVICE_COLUMNS
};

static gint
mouse_settings_device_get_int_property (XDevice *device,
                                        Atom     prop,
                                        guint    offset,
                                        gint    *horiz)
{
    Atom     type;
    gint     format;
    gulong   n_items, bytes_after;
    guchar  *data;
    gint     val = -1;
    gint     res;

    gdk_error_trap_push ();
    res = XGetDeviceProperty (GDK_DISPLAY (), device, prop, 0, 1000, False,
                              AnyPropertyType, &type, &format,
                              &n_items, &bytes_after, &data);
    if (gdk_error_trap_pop () == 0 && res == Success)
    {
        if (type == XA_INTEGER)
        {
            if (n_items > offset)
                val = data[offset];

            if (n_items > 1 + offset && horiz != NULL)
                *horiz = data[offset + 1];
        }

        XFree (data);
    }

    return val;
}

static gboolean
mouse_settings_device_get_selected (GtkBuilder  *dialog,
                                    XDevice    **device)
{
	GtkTreeIter   iter;
	gboolean      found = FALSE;
	gulong        xid;
	GtkTreeModel *model;

	/* get the selected item */
	found = gtk_combo_box_get_active_iter (GTK_COMBO_BOX (WID ("device-combobox")), &iter);
	if (found)
	{
		/* get the device id  */
		model = gtk_combo_box_get_model (GTK_COMBO_BOX (WID ("device-combobox")));
		gtk_tree_model_get (model, &iter, COLUMN_DEVICE_XID, &xid, -1);

		if (device != NULL)
		{
			/* open the device */
			gdk_error_trap_push ();
			*device = XOpenDevice (GDK_DISPLAY (), xid);
			if (gdk_error_trap_pop () != 0 || *device == NULL)
			{
				g_critical ("Unable to open device %ld", xid);
				*device = NULL;
				found = FALSE;
			}
		}
	}

	return found;
}

static void
mouse_settings_device_set_enabled (GtkToggleButton *button,
                                   GtkBuilder      *dialog)
{
	XDevice  *device;
	Atom      prop_enabled;
	gboolean  enabled;

	enabled = gtk_toggle_button_get_active (button);

	if (mouse_settings_device_get_selected(dialog, &device)) {
		prop_enabled = XInternAtom (GDK_DISPLAY_XDISPLAY(gdk_display_get_default()), "Device Enabled", False);

		if (!prop_enabled)
			return;

		unsigned char data = enabled;
		gdk_error_trap_push ();
		XChangeDeviceProperty (GDK_DISPLAY_XDISPLAY(gdk_display_get_default()), device,
		                       prop_enabled, XA_INTEGER, 8,
		                       PropModeReplace, &data, 1);
		gdk_flush ();
		if (gdk_error_trap_pop ()) {
			g_warning ("Error %s device.",
			           (enabled) ? "enabling" : "disabling");
		}

		/* close the device */
		XCloseDevice (GDK_DISPLAY (), device);
	}
}

static void
mouse_settings_device_selection_changed (GtkBuilder *dialog)
{
	XDevice   *device;
	Atom       prop_enabled;
	gint       is_enabled = -1;

	if (mouse_settings_device_get_selected(dialog, &device)) {
		prop_enabled = XInternAtom (GDK_DISPLAY_XDISPLAY(gdk_display_get_default()), "Device Enabled", False);

		if (!prop_enabled)
			return;

		is_enabled = mouse_settings_device_get_int_property (device, prop_enabled, 0, NULL);

		gtk_widget_set_sensitive (WID ("device-enabled"), is_enabled != -1);
		gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (WID ("device-enabled")), is_enabled > 0);

		/* close the device */
		XCloseDevice (GDK_DISPLAY (), device);
	}
}

static void
mouse_settings_device_populate_store (GtkBuilder *dialog,
                                      gboolean    create_store)
{
	XDeviceInfo     *device_list, *device_info;
	gint             ndevices;
	gint             i;
	GtkTreeIter      iter;
	GtkListStore    *store;
	GtkCellRenderer *renderer;

	/* create or get the store */
	if (G_LIKELY (create_store))
	{
		store = gtk_list_store_new (N_DEVICE_COLUMNS,
		                            G_TYPE_STRING /* COLUMN_DEVICE_NAME */,
		                            G_TYPE_ULONG /* COLUMN_DEVICE_XID */);

		gtk_combo_box_set_model (GTK_COMBO_BOX (WID ("device-combobox")), GTK_TREE_MODEL (store));

		/* text renderer */
		renderer = gtk_cell_renderer_text_new ();
		gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (WID ("device-combobox")), renderer, TRUE);
		gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (WID ("device-combobox")), renderer,
		                                "text", COLUMN_DEVICE_NAME, NULL);

		g_signal_connect_swapped (G_OBJECT (WID ("device-combobox")), "changed",
		                          G_CALLBACK (mouse_settings_device_selection_changed), dialog);
	}
	else
	{
		store = GTK_LIST_STORE (gtk_combo_box_get_model (GTK_COMBO_BOX (WID ("device-combobox"))));
		gtk_list_store_clear (store);
	}

	/* get all the registered devices */
	gdk_error_trap_push ();
	device_list = XListInputDevices (GDK_DISPLAY (), &ndevices);
	if (gdk_error_trap_pop () != 0 || device_list == NULL)
	{
		g_message ("No devices found");
		return;
	}

	for (i = 0; i < ndevices; i++)
	{
		/* get the device */
		device_info = &device_list[i];

		/* filter out the pointer and virtual devices */
		if (device_info->use != IsXExtensionPointer
		    || g_str_has_prefix (device_info->name, "Virtual core XTEST"))
			continue;

		/* cannot go any further without device name */
		if (device_info->name == NULL)
			continue;

		/* insert in the store */
		gtk_list_store_insert_with_values (store, &iter, i,
		                                   COLUMN_DEVICE_NAME, device_info->name,
		                                   COLUMN_DEVICE_XID, device_info->id,
		                                   -1);
	}

	XFreeDeviceList (device_list);

	gtk_combo_box_set_active (GTK_COMBO_BOX (WID ("device-combobox")), 0);
}

/* Set up the property editors in the dialog. */
static void
setup_dialog (GtkBuilder *dialog)
{
	GtkRadioButton    *radio;

	mouse_settings_device_populate_store(dialog, TRUE);
	g_signal_connect (G_OBJECT (WID("device-enabled")), "toggled",
	                  G_CALLBACK (mouse_settings_device_set_enabled), dialog);

	/* Orientation radio buttons */
	radio = GTK_RADIO_BUTTON (WID ("left_handed_radio"));
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON(radio),
		g_settings_get_boolean(mouse_settings, "left-handed"));
	/* explicitly connect to button-release so that you can change orientation with either button */
	g_signal_connect (WID ("right_handed_radio"), "button_release_event",
		G_CALLBACK (orientation_radio_button_release_event), NULL);
	g_signal_connect (WID ("left_handed_radio"), "button_release_event",
		G_CALLBACK (orientation_radio_button_release_event), NULL);
	g_signal_connect (WID ("left_handed_radio"), "toggled",
		G_CALLBACK (orientation_radio_button_toggled), dialog);

	/* Locate pointer toggle */
	g_settings_bind (mouse_settings, "locate-pointer", WID ("locate_pointer_toggle"),
		"active", G_SETTINGS_BIND_DEFAULT);

	/* Double-click time */
	g_settings_bind (mouse_settings, DOUBLE_CLICK_KEY,
		gtk_range_get_adjustment (GTK_RANGE (WID ("delay_scale"))), "value",
		G_SETTINGS_BIND_DEFAULT);
	
	gtk_image_set_from_stock (GTK_IMAGE (WID ("double_click_image")), MOUSE_DBLCLCK_OFF, mouse_capplet_dblclck_icon_get_size ());
	g_object_set_data (G_OBJECT (WID ("double_click_eventbox")), "image", WID ("double_click_image"));
	g_signal_connect (WID ("double_click_eventbox"), "button_press_event",
			  G_CALLBACK (event_box_button_press_event), NULL);

	/* speed */
	g_settings_bind (mouse_settings, "motion-acceleration",
		gtk_range_get_adjustment (GTK_RANGE (WID ("accel_scale"))), "value",
		G_SETTINGS_BIND_DEFAULT);
	g_settings_bind (mouse_settings, "motion-threshold",
		gtk_range_get_adjustment (GTK_RANGE (WID ("sensitivity_scale"))), "value",
		G_SETTINGS_BIND_DEFAULT);

	/* DnD threshold */
	g_settings_bind (mouse_settings, "drag-threshold",
		gtk_range_get_adjustment (GTK_RANGE (WID ("drag_threshold_scale"))), "value",
		G_SETTINGS_BIND_DEFAULT);

	/* Trackpad page */
	if (find_synaptics () == FALSE) {
		gtk_notebook_remove_page (GTK_NOTEBOOK (WID ("prefs_widget")), -1);
		gtk_notebook_set_show_tabs (GTK_NOTEBOOK (WID ("prefs_widget")), FALSE);
	}
	else {
		g_settings_bind (touchpad_settings, "disable-while-typing",
			WID ("disable_w_typing_toggle"), "active",
			G_SETTINGS_BIND_DEFAULT);
		g_settings_bind (touchpad_settings, "tap-to-click",
			WID ("tap_to_click_toggle"), "active",
			G_SETTINGS_BIND_DEFAULT);
		g_settings_bind (touchpad_settings, "horiz-scroll-enabled",
			WID ("horiz_scroll_toggle"), "active",
			G_SETTINGS_BIND_DEFAULT);

		scrollmethod_gsettings_changed_event (touchpad_settings, "scroll-method", dialog);

		radio = GTK_RADIO_BUTTON (WID ("scroll_disabled_radio"));
		GSList *radio_group = gtk_radio_button_get_group (radio);
		GSList *item = NULL;

		synaptics_check_capabilities (dialog);
		for (item = radio_group; item != NULL; item = item->next) {
			g_signal_connect (G_OBJECT (item->data), "clicked",
				  G_CALLBACK(scrollmethod_clicked_event),
				  dialog);
		}
		g_signal_connect (touchpad_settings,
			"changed::scroll-method",
			G_CALLBACK(scrollmethod_gsettings_changed_event),
			dialog);
	}

}

/* Construct the dialog */

static GtkBuilder *
create_dialog (void)
{
	GtkBuilder   *dialog;
	GtkSizeGroup *size_group;
	GError       *error = NULL;

	dialog = gtk_builder_new ();
	gtk_builder_add_from_file (dialog, MATECC_UI_DIR "/mate-mouse-properties.ui", &error);
	if (error != NULL) {
		g_warning ("Error loading UI file: %s", error->message);
		return NULL;
	}

	size_group = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);
	gtk_size_group_add_widget (size_group, WID ("acceleration_label"));
	gtk_size_group_add_widget (size_group, WID ("sensitivity_label"));
	gtk_size_group_add_widget (size_group, WID ("threshold_label"));
	gtk_size_group_add_widget (size_group, WID ("timeout_label"));

	size_group = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);
	gtk_size_group_add_widget (size_group, WID ("acceleration_fast_label"));
	gtk_size_group_add_widget (size_group, WID ("sensitivity_high_label"));
	gtk_size_group_add_widget (size_group, WID ("threshold_large_label"));
	gtk_size_group_add_widget (size_group, WID ("timeout_long_label"));

	size_group = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);
	gtk_size_group_add_widget (size_group, WID ("acceleration_slow_label"));
	gtk_size_group_add_widget (size_group, WID ("sensitivity_low_label"));
	gtk_size_group_add_widget (size_group, WID ("threshold_small_label"));
	gtk_size_group_add_widget (size_group, WID ("timeout_short_label"));

	return dialog;
}

/* Callback issued when a button is clicked on the dialog */

static void
dialog_response_cb (GtkDialog *dialog, gint response_id, gpointer data)
{
	if (response_id == GTK_RESPONSE_HELP)
		capplet_help (GTK_WINDOW (dialog),
			      "goscustperiph-5");
	else
		gtk_main_quit ();
}

int
main (int argc, char **argv)
{
	GtkBuilder     *dialog;
	GtkWidget      *dialog_win, *w;
	gchar *start_page = NULL;

	GOptionContext *context;
	GOptionEntry cap_options[] = {
		{"show-page", 'p', G_OPTION_FLAG_IN_MAIN,
		 G_OPTION_ARG_STRING,
		 &start_page,
		 /* TRANSLATORS: don't translate the terms in brackets */
		 N_("Specify the name of the page to show (general)"),
		 N_("page") },
		{NULL}
	};

	context = g_option_context_new (_("- MATE Mouse Preferences"));
	g_option_context_add_main_entries (context, cap_options, GETTEXT_PACKAGE);
	capplet_init (context, &argc, &argv);

	capplet_init_stock_icons ();

	activate_settings_daemon ();

	mouse_settings = g_settings_new (MOUSE_SCHEMA);
	touchpad_settings = g_settings_new (TOUCHPAD_SCHEMA);

	dialog = create_dialog ();

	if (dialog) {
		setup_dialog (dialog);
		//setup_accessibility (dialog);

		dialog_win = WID ("mouse_properties_dialog");
		g_signal_connect (dialog_win, "response",
				  G_CALLBACK (dialog_response_cb), NULL);

		if (start_page != NULL) {
			gchar *page_name;

			page_name = g_strconcat (start_page, "_vbox", NULL);
			g_free (start_page);

			w = WID (page_name);
			if (w != NULL) {
				GtkNotebook *nb;
				gint pindex;

				nb = GTK_NOTEBOOK (WID ("prefs_widget"));
				pindex = gtk_notebook_page_num (nb, w);
				if (pindex != -1)
					gtk_notebook_set_current_page (nb, pindex);
			}
			g_free (page_name);
		}

		capplet_set_icon (dialog_win, "input-mouse");
		gtk_widget_show (dialog_win);

		gtk_main ();

		g_object_unref (dialog);
	}

	g_object_unref (mouse_settings);
	g_object_unref (touchpad_settings);

	return 0;
}
