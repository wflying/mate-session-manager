/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2006 Vincent Untz
 * Copyright (C) 2008 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 * Authors:
 *	Vincent Untz <vuntz@gnome.org>
 */

#include <config.h>

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <upower.h>

#include "gsm-logout-dialog.h"
#ifdef HAVE_SYSTEMD
#include "gsm-systemd.h"
#include <systemd/sd-daemon.h>
#endif
#include "gsm-consolekit.h"
#include "mdm.h"

#define GSM_LOGOUT_DIALOG_GET_PRIVATE(o)                                \
        (G_TYPE_INSTANCE_GET_PRIVATE ((o), GSM_TYPE_LOGOUT_DIALOG, GsmLogoutDialogPrivate))

#define AUTOMATIC_ACTION_TIMEOUT 60

#define GSM_ICON_LOGOUT   "system-log-out"
#define GSM_ICON_SHUTDOWN "system-shutdown"

typedef enum {
        GSM_DIALOG_LOGOUT_TYPE_LOGOUT,
        GSM_DIALOG_LOGOUT_TYPE_SHUTDOWN
} GsmDialogLogoutType;

struct _GsmLogoutDialogPrivate
{
        GsmDialogLogoutType  type;

        UpClient            *up_client;
#ifdef HAVE_SYSTEMD
        GsmSystemd          *systemd;
#endif
        GsmConsolekit       *consolekit;

        int                  timeout;
        unsigned int         timeout_id;

        unsigned int         default_response;
};

static GsmLogoutDialog *current_dialog = NULL;

static void gsm_logout_dialog_set_timeout  (GsmLogoutDialog *logout_dialog);

static void gsm_logout_dialog_destroy  (GsmLogoutDialog *logout_dialog,
                                        gpointer         data);

static void gsm_logout_dialog_show     (GsmLogoutDialog *logout_dialog,
                                        gpointer         data);

enum {
        PROP_0,
        PROP_MESSAGE_TYPE
};

G_DEFINE_TYPE (GsmLogoutDialog, gsm_logout_dialog, GTK_TYPE_MESSAGE_DIALOG);

static void
gsm_logout_dialog_set_property (GObject      *object,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
        switch (prop_id) {
        case PROP_MESSAGE_TYPE:
                break;
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static void
gsm_logout_dialog_get_property (GObject     *object,
                                guint        prop_id,
                                GValue      *value,
                                GParamSpec  *pspec)
{
        switch (prop_id) {
        case PROP_MESSAGE_TYPE:
                g_value_set_enum (value, GTK_MESSAGE_WARNING);
                break;
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static void
gsm_logout_dialog_class_init (GsmLogoutDialogClass *klass)
{
        GObjectClass *gobject_class;

        gobject_class = G_OBJECT_CLASS (klass);

        /* This is a workaround to avoid a stupid crash: libmateui
         * listens for the "show" signal on all GtkMessageDialog and
         * gets the "message-type" of the dialogs. We will crash when
         * it accesses this property if we don't override it since we
         * didn't define it. */
        gobject_class->set_property = gsm_logout_dialog_set_property;
        gobject_class->get_property = gsm_logout_dialog_get_property;

        g_object_class_override_property (gobject_class,
                                          PROP_MESSAGE_TYPE,
                                          "message-type");

        g_type_class_add_private (klass, sizeof (GsmLogoutDialogPrivate));
}

static void
gsm_logout_dialog_init (GsmLogoutDialog *logout_dialog)
{
        logout_dialog->priv = GSM_LOGOUT_DIALOG_GET_PRIVATE (logout_dialog);

        logout_dialog->priv->timeout_id = 0;
        logout_dialog->priv->timeout = 0;
        logout_dialog->priv->default_response = GTK_RESPONSE_CANCEL;

        gtk_window_set_skip_taskbar_hint (GTK_WINDOW (logout_dialog), TRUE);
        gtk_window_set_keep_above (GTK_WINDOW (logout_dialog), TRUE);
        gtk_window_stick (GTK_WINDOW (logout_dialog));

        logout_dialog->priv->up_client = up_client_new ();

#ifdef HAVE_SYSTEMD
        if (sd_booted() > 0)
            logout_dialog->priv->systemd = gsm_get_systemd ();
        else
#endif
        logout_dialog->priv->consolekit = gsm_get_consolekit ();

        g_signal_connect (logout_dialog,
                          "destroy",
                          G_CALLBACK (gsm_logout_dialog_destroy),
                          NULL);

        g_signal_connect (logout_dialog,
                          "show",
                          G_CALLBACK (gsm_logout_dialog_show),
                          NULL);
}

static void
gsm_logout_dialog_destroy (GsmLogoutDialog *logout_dialog,
                           gpointer         data)
{
        if (logout_dialog->priv->timeout_id != 0) {
                g_source_remove (logout_dialog->priv->timeout_id);
                logout_dialog->priv->timeout_id = 0;
        }

        if (logout_dialog->priv->up_client) {
                g_object_unref (logout_dialog->priv->up_client);
                logout_dialog->priv->up_client = NULL;
        }

#ifdef HAVE_SYSTEMD
        if (logout_dialog->priv->systemd) {
                g_object_unref (logout_dialog->priv->systemd);
                logout_dialog->priv->systemd = NULL;
        }
#endif

        if (logout_dialog->priv->consolekit) {
                g_object_unref (logout_dialog->priv->consolekit);
                logout_dialog->priv->consolekit = NULL;
        }

        current_dialog = NULL;
}

static gboolean
gsm_logout_supports_system_suspend (GsmLogoutDialog *logout_dialog)
{
        return up_client_get_can_suspend (logout_dialog->priv->up_client);
}

static gboolean
gsm_logout_supports_system_hibernate (GsmLogoutDialog *logout_dialog)
{
        return up_client_get_can_hibernate (logout_dialog->priv->up_client);
}

static gboolean
gsm_logout_supports_switch_user (GsmLogoutDialog *logout_dialog)
{
        gboolean ret;

#ifdef HAVE_SYSTEMD
        if (sd_booted () > 0)
            ret = gsm_systemd_can_switch_user (logout_dialog->priv->systemd);
        else
#endif
        ret = gsm_consolekit_can_switch_user (logout_dialog->priv->consolekit);

        return ret;
}

static gboolean
gsm_logout_supports_reboot (GsmLogoutDialog *logout_dialog)
{
        gboolean ret;

#ifdef HAVE_SYSTEMD
        if (sd_booted () > 0)
            ret = gsm_systemd_can_restart (logout_dialog->priv->systemd);
        else
#endif
        ret = gsm_consolekit_can_restart (logout_dialog->priv->consolekit);
        if (!ret) {
                ret = mdm_supports_logout_action (MDM_LOGOUT_ACTION_REBOOT);
        }

        return ret;
}

static gboolean
gsm_logout_supports_shutdown (GsmLogoutDialog *logout_dialog)
{
        gboolean ret;

#ifdef HAVE_SYSTEMD
        if (sd_booted () > 0)
            ret = gsm_systemd_can_stop (logout_dialog->priv->systemd);
        else
#endif
        ret = gsm_consolekit_can_stop (logout_dialog->priv->consolekit);

        if (!ret) {
                ret = mdm_supports_logout_action (MDM_LOGOUT_ACTION_SHUTDOWN);
        }

        return ret;
}

static void
gsm_logout_dialog_show (GsmLogoutDialog *logout_dialog, gpointer user_data)
{
        gsm_logout_dialog_set_timeout (logout_dialog);
}

static gboolean
gsm_logout_dialog_timeout (gpointer data)
{
        GsmLogoutDialog *logout_dialog;
        char            *seconds_warning;
        char            *secondary_text;
        int              seconds_to_show;
        static char     *session_type = NULL;
        static gboolean  is_not_login;

        logout_dialog = (GsmLogoutDialog *) data;

        if (!logout_dialog->priv->timeout) {
                gtk_dialog_response (GTK_DIALOG (logout_dialog),
                                     logout_dialog->priv->default_response);

                return FALSE;
        }

        if (logout_dialog->priv->timeout <= 30) {
                seconds_to_show = logout_dialog->priv->timeout;
        } else {
                seconds_to_show = (logout_dialog->priv->timeout/10) * 10;

                if (logout_dialog->priv->timeout % 10)
                        seconds_to_show += 10;
        }

        switch (logout_dialog->priv->type) {
        case GSM_DIALOG_LOGOUT_TYPE_LOGOUT:
                seconds_warning = ngettext ("You will be automatically logged "
                                            "out in %d second.",
                                            "You will be automatically logged "
                                            "out in %d seconds.",
                                            seconds_to_show);
                break;

        case GSM_DIALOG_LOGOUT_TYPE_SHUTDOWN:
                seconds_warning = ngettext ("This system will be automatically "
                                            "shut down in %d second.",
                                            "This system will be automatically "
                                            "shut down in %d seconds.",
                                            seconds_to_show);
                break;

        default:
                g_assert_not_reached ();
        }

        if (session_type == NULL) {
#ifdef HAVE_SYSTEMD
                if (sd_booted () > 0) {
                    GsmSystemd *systemd;
                    systemd = gsm_get_systemd ();
                    session_type = gsm_systemd_get_current_session_type (systemd);
                    g_object_unref (systemd);
                    is_not_login = (g_strcmp0 (session_type, GSM_SYSTEMD_SESSION_TYPE_LOGIN_WINDOW) != 0);
                }
                else {
#endif
                GsmConsolekit *consolekit;
                consolekit = gsm_get_consolekit ();
                session_type = gsm_consolekit_get_current_session_type (consolekit);
                g_object_unref (consolekit);
                is_not_login = (g_strcmp0 (session_type, GSM_CONSOLEKIT_SESSION_TYPE_LOGIN_WINDOW) != 0);
#ifdef HAVE_SYSTEMD
                }
#endif
        }

        if (is_not_login) {
                char *name, *tmp;

                name = g_locale_to_utf8 (g_get_real_name (), -1, NULL, NULL, NULL);

                if (!name || name[0] == '\0' || strcmp (name, "Unknown") == 0) {
                        name = g_locale_to_utf8 (g_get_user_name (), -1 , NULL, NULL, NULL);
                }

                if (!name) {
                        name = g_strdup (g_get_user_name ());
                }

                tmp = g_strdup_printf (_("You are currently logged in as \"%s\"."), name);
                secondary_text = g_strconcat (tmp, "\n", seconds_warning, NULL);
                g_free (tmp);

                g_free (name);
        } else {
                secondary_text = g_strdup (seconds_warning);
        }

        gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (logout_dialog),
                                                  secondary_text,
                                                  seconds_to_show,
                                                  NULL);

        logout_dialog->priv->timeout--;

        g_free (secondary_text);

        return TRUE;
}

static void
gsm_logout_dialog_set_timeout (GsmLogoutDialog *logout_dialog)
{
        logout_dialog->priv->timeout = AUTOMATIC_ACTION_TIMEOUT;

        /* Sets the secondary text */
        gsm_logout_dialog_timeout (logout_dialog);

        if (logout_dialog->priv->timeout_id != 0) {
                g_source_remove (logout_dialog->priv->timeout_id);
        }

        logout_dialog->priv->timeout_id = g_timeout_add (1000,
                                                         gsm_logout_dialog_timeout,
                                                         logout_dialog);
}



/* add by cs2c */
static void update_widget_bg(GtkWidget *widget, gchar *img_file)
{
        GtkStyle *style;
        GdkPixbuf *pixbuf;
        GdkPixmap *pixmap;
        gint width, height;

        pixbuf = gdk_pixbuf_new_from_file(img_file, NULL);
        width = gdk_pixbuf_get_width(pixbuf);
        height = gdk_pixbuf_get_height(pixbuf);
        pixmap = gdk_pixmap_new(NULL, width, height, 24);
        gdk_pixbuf_render_pixmap_and_mask(pixbuf, &pixmap, NULL, 0);
        style = gtk_style_copy(GTK_WIDGET (widget)->style);

        if (style->bg_pixmap[GTK_STATE_NORMAL])
                g_object_unref(style->bg_pixmap[GTK_STATE_NORMAL]);

        style->bg_pixmap[GTK_STATE_NORMAL] = g_object_ref(pixmap);
        style->bg_pixmap[GTK_STATE_ACTIVE] = g_object_ref(pixmap);
        style->bg_pixmap[GTK_STATE_PRELIGHT] = g_object_ref(pixmap);
        style->bg_pixmap[GTK_STATE_SELECTED] = g_object_ref(pixmap);
        style->bg_pixmap[GTK_STATE_INSENSITIVE] = g_object_ref(pixmap);
        gtk_widget_set_style(GTK_WIDGET (widget), style);
        g_object_unref(style);
}

/* add by cs2c */
static void destroy_win (GtkWidget *button, gpointer userdata)
{
        GtkWidget *w = (GtkWidget *)userdata;
        gtk_widget_destroy(w);
        //g_signal_connect(w, "destroy", G_CALLBACK(gtk_widget_destroy), &w);
}

/* add by cs2c */
GtkWidget *
win_shutdown_dialog (GdkScreen *screen, GsmManager *manager)
{

        GtkWidget *label1;
	gchar *str_close = NULL;
	gchar *str_suspend = NULL;
	gchar *str_hibernate = NULL;
	gchar *str_shutdown = NULL;
	gchar *str_restart = NULL;
        GtkWidget *bt_hibernate, *bt_suspend, *label_hibernate, *label_suspend, *bt_shutdown, *label_shutdown, *bt_reboot, *label_reboot;
        GtkWidget *image_hibernate, *image_suspend, *image_shutdown, *image_reboot;

        GtkWidget *image, *logo;

        GtkWidget *table;
        GtkWidget *win;
        GtkWidget *bt_cancel;
        GtkBorder *border_reboot;

        image=gtk_image_new_from_file("/usr/share/icons/hicolor/36x36/apps/gs_bg_shutdown.jpg");
        logo=gtk_image_new_from_file("/usr/share/icons/hicolor/36x36/apps/gs_logo.png");

        image_hibernate = gtk_image_new_from_file("/usr/share/icons/hicolor/36x36/apps/gs_hibernate.png");
        image_suspend = gtk_image_new_from_file("/usr/share/icons/hicolor/36x36/apps/gs_suspend.png");
        image_shutdown = gtk_image_new_from_file("/usr/share/icons/hicolor/36x36/apps/gs_shutdown.png");
        image_reboot = gtk_image_new_from_file("/usr/share/icons/hicolor/36x36/apps/gs_reboot.png");

        win=gtk_window_new(GTK_WINDOW_POPUP);

        g_signal_connect(win, "destroy", G_CALLBACK(gtk_widget_destroy), &win);
        gtk_window_set_title(GTK_WINDOW(win), "Shutdown Computer");
        gtk_window_set_default_size(GTK_WINDOW(win), 377, 241);
        gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_CENTER);
        gtk_window_set_screen (GTK_WINDOW (win), screen);
        gtk_container_set_border_width(GTK_CONTAINER(win), 4);
        bt_cancel = gtk_button_new_with_label(_("Cancel"));
        gtk_widget_set_size_request(bt_cancel, 54, 24);
        g_signal_connect(G_OBJECT(bt_cancel), "clicked", G_CALLBACK(destroy_win), win);
        update_widget_bg(win, "/usr/share/icons/hicolor/36x36/apps/gs_bg_shutdown.jpg");

        label1 = gtk_label_new(NULL);
        //label1 = gtk_label_new(_("Close Computer"));
        str_close = g_strdup_printf("<span foreground='#ffffff' size='15000'><b>%s</b></span>",_("Close Computer"));
        gtk_label_set_markup((GtkLabel *)(label1), str_close);

        table = gtk_table_new(4, 11, TRUE);
        gtk_table_set_row_spacing(GTK_TABLE(table), 0, 40);
        gtk_table_set_row_spacing(GTK_TABLE(table), 3, 40);

        bt_hibernate = gtk_button_new();
        gtk_button_set_image(GTK_BUTTON(bt_hibernate), image_hibernate);
        g_signal_connect(G_OBJECT(bt_hibernate), "clicked", G_CALLBACK(request_hibernate_cb), manager);
        label_hibernate = gtk_label_new(NULL);
        str_hibernate = g_strdup_printf("<span foreground='#ffffff' size='10000'><b>%s</b></span>",_("_Hibernate"));
        gtk_label_set_markup((GtkLabel *)(label_hibernate), str_hibernate);
        //gtk_label_set_markup((GtkLabel *)label_suspend, "<span foreground='#ffffff' size='10000'><b>待机(U)</b></span>");
        gtk_container_set_border_width (GTK_CONTAINER (bt_hibernate), 0);

        bt_suspend = gtk_button_new();
        gtk_button_set_image(GTK_BUTTON(bt_suspend), image_suspend);
        g_signal_connect(G_OBJECT(bt_suspend), "clicked", G_CALLBACK(request_suspend_cb), manager);
        label_suspend = gtk_label_new(NULL);
        str_suspend = g_strdup_printf("<span foreground='#ffffff' size='10000'><b>%s</b></span>",_("S_uspend"));
        gtk_label_set_markup((GtkLabel *)(label_suspend), str_suspend);
        //gtk_label_set_markup((GtkLabel *)label_suspend, "<span foreground='#ffffff' size='10000'><b>å¾æº(U)</b></span>");
        gtk_container_set_border_width (GTK_CONTAINER (bt_suspend), 0);

        bt_shutdown = gtk_button_new();
        gtk_button_set_image(GTK_BUTTON(bt_shutdown), image_shutdown);
        g_signal_connect(G_OBJECT(bt_shutdown), "clicked", G_CALLBACK(request_shutdown_cb), manager);
        label_shutdown = gtk_label_new(NULL);
        str_shutdown = g_strdup_printf("<span foreground='#ffffff' size='10000'><b>%s</b></span>",_("_Shut Down"));
        gtk_label_set_markup((GtkLabel *)(label_shutdown), str_shutdown);
        //gtk_label_set_markup((GtkLabel *)label_shutdown, "<span foreground='#ffffff' size='10000'><b>å³é­(S)</b></span>");
        gtk_container_set_border_width (GTK_CONTAINER (bt_shutdown), 0);

        bt_reboot = gtk_button_new();
        gtk_button_set_image(GTK_BUTTON(bt_reboot), image_reboot);
        g_signal_connect(G_OBJECT(bt_reboot), "clicked", G_CALLBACK(request_reboot_cb), manager);
        label_reboot = gtk_label_new(NULL);
        str_restart = g_strdup_printf("<span foreground='#ffffff' size='10000'><b>%s</b></span>",_("_Restart"));
        gtk_label_set_markup((GtkLabel *)(label_reboot), str_restart);
        //gtk_label_set_markup((GtkLabel *)label_reboot, "<span foreground='#ffffff' size='10000'><b>éæ°å¯å¨(R)</b></span>");
        gtk_container_set_border_width (GTK_CONTAINER (bt_reboot), 0);

        gtk_table_attach(GTK_TABLE(table),label1,0,4,0,1, GTK_EXPAND, GTK_EXPAND, 1, 1);
        gtk_table_attach(GTK_TABLE(table),logo,12,14,0,1, GTK_EXPAND, GTK_EXPAND, 1, 1);

        gtk_table_attach(GTK_TABLE(table),bt_suspend,1,4,1,2, GTK_EXPAND, GTK_EXPAND, 1, 1);
        gtk_table_attach(GTK_TABLE(table),bt_hibernate,4,7,1,2, GTK_EXPAND, GTK_EXPAND, 1, 1);
        gtk_table_attach(GTK_TABLE(table),bt_shutdown,7,10,1,2, GTK_EXPAND, GTK_EXPAND, 1, 1);
        gtk_table_attach(GTK_TABLE(table),bt_reboot,10,13,1,2, GTK_EXPAND, GTK_EXPAND, 1, 1);

        gtk_table_attach(GTK_TABLE(table),label_suspend,1,4,2,3, GTK_EXPAND, GTK_EXPAND, 1, 1);
        gtk_table_attach(GTK_TABLE(table),label_hibernate,4,7,2,3, GTK_EXPAND, GTK_EXPAND, 1, 1);
        gtk_table_attach(GTK_TABLE(table),label_shutdown,7,10,2,3, GTK_EXPAND, GTK_EXPAND, 1, 1);
        gtk_table_attach(GTK_TABLE(table),label_reboot,10,13,2,3, GTK_EXPAND, GTK_EXPAND, 1, 1);

        gtk_table_attach(GTK_TABLE(table),bt_cancel,11,14,3,4, GTK_EXPAND, GTK_EXPAND, 1, 1);

        gtk_container_add(GTK_CONTAINER(win), table);

        gtk_widget_show_all(win);
	
	return win;
}

/* add by cs2c */
GtkWidget *
win_logout_dialog (GdkScreen *screen, GsmManager *manager)
{

        GtkWidget *label1;
	gchar *str_Logout = NULL;
        gchar *str_switch = NULL;
        gchar *str_logout = NULL;
        GtkWidget *bt_switch, *label_switch, *bt_logout, *label_logout;
        GtkWidget *image_switch, *image_logout;
        GtkWidget *label1_align, *logo_align;

        GtkWidget *image, *logo;

        GtkWidget *table;
        GtkWidget *win;
        GtkWidget *bt_cancel;
        GtkBorder *border_logout;

        image=gtk_image_new_from_file("/usr/share/icons/hicolor/36x36/apps/gs_bg_logout.jpg");
        logo=gtk_image_new_from_file("/usr/share/icons/hicolor/36x36/apps/gs_logo.png");

        image_switch = gtk_image_new_from_file("/usr/share/icons/hicolor/36x36/apps/gs_switch.png");
        image_logout = gtk_image_new_from_file("/usr/share/icons/hicolor/36x36/apps/gs_logout.png");

        win=gtk_window_new(GTK_WINDOW_POPUP);

        g_signal_connect(win, "destroy", G_CALLBACK(gtk_widget_destroy), &win);
        gtk_window_set_title(GTK_WINDOW(win), "Logout Windows");
        gtk_window_set_default_size(GTK_WINDOW(win), 377, 241);
        gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_CENTER);
        gtk_container_set_border_width(GTK_CONTAINER(win), 4);
        //bt_cancel = gtk_button_new_with_label("åæ¶(C)");
        bt_cancel = gtk_button_new_with_label(_("Cancel"));
        gtk_widget_set_size_request(bt_cancel, 54, 24);
        g_signal_connect(G_OBJECT(bt_cancel), "clicked", G_CALLBACK(destroy_win), win);
        update_widget_bg(win, "/usr/share/icons/hicolor/36x36/apps/gs_bg_logout.jpg");

        label1 = gtk_label_new(NULL);
        str_Logout = g_strdup_printf("<span foreground='#ffffff' size='15000'><b>%s</b></span>",_("Log Out"));
        gtk_label_set_markup((GtkLabel *)(label1), str_Logout);
        //gtk_label_set_markup((GtkLabel *)label1, "<span foreground='#ffffff' size='15000'><b>æ³¨é Windows</b></span>");
        label1_align = gtk_alignment_new (0, 1, 0, 0);
        gtk_container_add(GTK_CONTAINER(label1_align), label1);

        table = gtk_table_new(4, 8, TRUE);
        gtk_table_set_row_spacing(GTK_TABLE(table), 0, 40);
        gtk_table_set_row_spacing(GTK_TABLE(table), 3, 40);

        bt_switch = gtk_button_new();
        gtk_button_set_image(GTK_BUTTON(bt_switch), image_switch);
        g_signal_connect(G_OBJECT(bt_switch), "clicked", G_CALLBACK(request_switch_cb), manager);
        label_switch = gtk_label_new(NULL);
        str_switch = g_strdup_printf("<span foreground='#ffffff' size='10000'><b>%s</b></span>",_("_Switch User"));
        gtk_label_set_markup((GtkLabel *)(label_switch), str_switch);
        //gtk_label_set_markup((GtkLabel *)label_switch, "<span foreground='#ffffff' size='10000'><b>åæ¢ç¨æ·(S)</b></span>");
        gtk_container_set_border_width (GTK_CONTAINER (bt_switch), 0);

        bt_logout = gtk_button_new();
        gtk_button_set_image(GTK_BUTTON(bt_logout), image_logout);
        g_signal_connect(G_OBJECT(bt_logout), "clicked", G_CALLBACK(request_logout_cb), manager);
        label_logout = gtk_label_new(NULL);
        str_logout = g_strdup_printf("<span foreground='#ffffff' size='10000'><b>%s</b></span>",_("_Log Out"));
        gtk_label_set_markup((GtkLabel *)(label_logout), str_logout);
        //gtk_label_set_markup((GtkLabel *)label_logout, "<span foreground='#ffffff' size='10000'><b>æ³¨é(L)</b></span>");
        gtk_container_set_border_width (GTK_CONTAINER (bt_logout), 0);

        gtk_table_attach(GTK_TABLE(table),label1_align,0,2,0,1, GTK_EXPAND, GTK_EXPAND, 1, 1);
        gtk_table_attach(GTK_TABLE(table),logo,6,8,0,1, GTK_EXPAND, GTK_EXPAND, 1, 1);

        gtk_table_attach(GTK_TABLE(table),bt_switch,1,4,1,2, GTK_EXPAND, GTK_EXPAND, 1, 1);
        gtk_table_attach(GTK_TABLE(table),bt_logout,4,7,1,2, GTK_EXPAND, GTK_EXPAND, 1, 1);

        gtk_table_attach(GTK_TABLE(table),label_switch,1,4,2,3, GTK_EXPAND, GTK_EXPAND, 1, 1);
        gtk_table_attach(GTK_TABLE(table),label_logout,4,7,2,3, GTK_EXPAND, GTK_EXPAND, 1, 1);

        gtk_table_attach(GTK_TABLE(table),bt_cancel,6,8,3,4, GTK_EXPAND, GTK_EXPAND, 1, 1);

        gtk_container_add(GTK_CONTAINER(win), table);

        gtk_widget_show_all(win);

	return win;
}



static GtkWidget *
gsm_get_dialog (GsmDialogLogoutType type,
                GdkScreen          *screen,
                guint32             activate_time)
{
        GsmLogoutDialog *logout_dialog;
        GtkWidget       *dialog_image;
        const char      *primary_text;
        const char      *icon_name;

        if (current_dialog != NULL) {
                gtk_widget_destroy (GTK_WIDGET (current_dialog));
        }

        logout_dialog = g_object_new (GSM_TYPE_LOGOUT_DIALOG, NULL);

        current_dialog = logout_dialog;

        gtk_window_set_title (GTK_WINDOW (logout_dialog), "");

        logout_dialog->priv->type = type;

        icon_name = NULL;
        primary_text = NULL;

        switch (type) {
        case GSM_DIALOG_LOGOUT_TYPE_LOGOUT:
                icon_name    = GSM_ICON_LOGOUT;
                primary_text = _("Log out of this system now?");

                logout_dialog->priv->default_response = GSM_LOGOUT_RESPONSE_LOGOUT;

                if (gsm_logout_supports_switch_user (logout_dialog)) {
                        gtk_dialog_add_button (GTK_DIALOG (logout_dialog),
                                               _("_Switch User"),
                                               GSM_LOGOUT_RESPONSE_SWITCH_USER);
                }

                gtk_dialog_add_button (GTK_DIALOG (logout_dialog),
                                       GTK_STOCK_CANCEL,
                                       GTK_RESPONSE_CANCEL);

                gtk_dialog_add_button (GTK_DIALOG (logout_dialog),
                                       _("_Log Out"),
                                       GSM_LOGOUT_RESPONSE_LOGOUT);

                break;
        case GSM_DIALOG_LOGOUT_TYPE_SHUTDOWN:
                icon_name    = GSM_ICON_SHUTDOWN;
                primary_text = _("Shut down this system now?");

                logout_dialog->priv->default_response = GSM_LOGOUT_RESPONSE_SHUTDOWN;

                if (gsm_logout_supports_system_suspend (logout_dialog)) {
                        gtk_dialog_add_button (GTK_DIALOG (logout_dialog),
                                               _("S_uspend"),
                                               GSM_LOGOUT_RESPONSE_SLEEP);
                }

                if (gsm_logout_supports_system_hibernate (logout_dialog)) {
                        gtk_dialog_add_button (GTK_DIALOG (logout_dialog),
                                               _("_Hibernate"),
                                               GSM_LOGOUT_RESPONSE_HIBERNATE);
                }

                if (gsm_logout_supports_reboot (logout_dialog)) {
                        gtk_dialog_add_button (GTK_DIALOG (logout_dialog),
                                               _("_Restart"),
                                               GSM_LOGOUT_RESPONSE_REBOOT);
                }

                gtk_dialog_add_button (GTK_DIALOG (logout_dialog),
                                       GTK_STOCK_CANCEL,
                                       GTK_RESPONSE_CANCEL);

                if (gsm_logout_supports_shutdown (logout_dialog)) {
                        gtk_dialog_add_button (GTK_DIALOG (logout_dialog),
                                               _("_Shut Down"),
                                               GSM_LOGOUT_RESPONSE_SHUTDOWN);
                }
                break;
        default:
                g_assert_not_reached ();
        }

        dialog_image = gtk_message_dialog_get_image (GTK_MESSAGE_DIALOG (logout_dialog));

        gtk_image_set_from_icon_name (GTK_IMAGE (dialog_image),
                                      icon_name, GTK_ICON_SIZE_DIALOG);
        gtk_window_set_icon_name (GTK_WINDOW (logout_dialog), icon_name);
        gtk_window_set_position (GTK_WINDOW (logout_dialog), GTK_WIN_POS_CENTER_ALWAYS);
        gtk_message_dialog_set_markup (GTK_MESSAGE_DIALOG (logout_dialog), primary_text);

        gtk_dialog_set_default_response (GTK_DIALOG (logout_dialog),
                                         logout_dialog->priv->default_response);

        gtk_window_set_screen (GTK_WINDOW (logout_dialog), screen);

        return GTK_WIDGET (logout_dialog);
}

GtkWidget *
gsm_get_shutdown_dialog (GdkScreen *screen,
                         guint32    activate_time)
{
#if 0
        return gsm_get_dialog (GSM_DIALOG_LOGOUT_TYPE_SHUTDOWN,
                               screen,
                               activate_time);
#endif
       return NULL;
}

GtkWidget *
gsm_get_logout_dialog (GdkScreen *screen,
                       guint32    activate_time)
{
#if 0
        return gsm_get_dialog (GSM_DIALOG_LOGOUT_TYPE_LOGOUT,
                               screen,
                               activate_time);
#endif
       return NULL;
}
