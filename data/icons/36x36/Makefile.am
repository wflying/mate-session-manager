size = 36x36

iconsdir = $(datadir)/icons/hicolor/$(size)/apps

icons_DATA = gs_bg_shutdown.jpg \
             gs_logo.png \
             gs_suspend.png \
             gs_hibernate.png \
             gs_shutdown.png \
             gs_reboot.png \
             gs_bg_logout.jpg \
             gs_logout.png \
             gs_switch.png
icons_SOURCE = 

EXTRA_DIST =		\
	$(icons_DATA)	\
	$(icons_SOURCE)

gtk_update_icon_cache = gtk-update-icon-cache -f -t $(datadir)/icons/hicolor

install-data-hook:
	@-if test -z "$(DESTDIR)"; then \
		echo "Updating Gtk icon cache."; \
		$(gtk_update_icon_cache); \
	else \
		echo "*** Icon cache not updated.  After install, run this:"; \
		echo "***   $(gtk_update_icon_cache)"; \
	fi

-include $(top_srcdir)/git.mk
