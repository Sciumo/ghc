# -----------------------------------------------------------------------------
#
# (c) 2009 The University of Glasgow
#
# This file is part of the GHC build system.
#
# To understand how the build system works and how to modify it, see
#      http://hackage.haskell.org/trac/ghc/wiki/Building/Architecture
#      http://hackage.haskell.org/trac/ghc/wiki/Building/Modifying
#
# -----------------------------------------------------------------------------

ifneq "$(Windows)" "YES"

install: install_driver_ghci

.PHONY: install_driver_ghci
install_driver_ghci: WRAPPER=$(DESTDIR)$(bindir)/ghci-$(ProjectVersion)
install_driver_ghci:
	$(call INSTALL_DIR,"$(DESTDIR)$(bindir)")
	"$(RM)" $(RM_OPTS)                                 "$(WRAPPER)"
	$(CREATE_SCRIPT)                                   "$(WRAPPER)"
	echo '#!$(SHELL)'                               >> "$(WRAPPER)"
	echo 'exec "$(bindir)/ghc-$(ProjectVersion)" --interactive $${1+"$$@"}' >> "$(WRAPPER)"
	$(EXECUTABLE_FILE)                                 "$(WRAPPER)"
	"$(RM)" $(RM_OPTS) "$(DESTDIR)$(bindir)/ghci"
	$(LN_S) ghci-$(ProjectVersion) "$(DESTDIR)$(bindir)/ghci"

else # Windows...

driver/ghci_dist_C_SRCS  = ghci.c ../utils/cwrapper.c ../utils/getLocation.c
driver/ghci_dist_CC_OPTS += -I driver/utils
driver/ghci_dist_PROG    = ghci$(exeext)
driver/ghci_dist_INSTALL = YES
driver/ghci_dist_OTHER_OBJS = driver/ghci/ghci.res

$(eval $(call build-prog,driver/ghci,dist,0))

driver/ghci_dist_PROG_VER = ghci-$(ProjectVersion)$(exeext)

INSTALL_BINS += driver/ghci/dist/build/tmp/$(driver/ghci_dist_PROG_VER)

driver/ghci/ghci.res : driver/ghci/ghci.rc driver/ghci/ghci.ico
	$(INPLACE_MINGW)/bin/windres --preprocessor="$(CPP) -xc -DRC_INVOKED" -o driver/ghci/ghci.res -i driver/ghci/ghci.rc -O coff

driver/ghci/dist/build/tmp/$(driver/ghci_dist_PROG_VER) : driver/ghci/dist/build/tmp/$(driver/ghci_dist_PROG)
	"$(CP)" $< $@

install : install_driver_ghcii

.PHONY: install_driver_ghcii
install_driver_ghcii: GHCII_SCRIPT=$(DESTDIR)$(bindir)/ghcii.sh
install_driver_ghcii: GHCII_SCRIPT_VERSIONED = $(DESTDIR)$(bindir)/ghcii-$(ProjectVersion).sh
install_driver_ghcii:
	$(call INSTALL_DIR,$(DESTDIR)$(bindir))
	"$(RM)" $(RM_OPTS) $(GHCII_SCRIPT)
	echo "#!$(SHELL)"                                  >> $(GHCII_SCRIPT)
	echo 'exec "$$0"/../ghc --interactive $${1+"$$@"}' >> $(GHCII_SCRIPT)
	$(EXECUTABLE_FILE) $(GHCII_SCRIPT)
	cp $(GHCII_SCRIPT) $(GHCII_SCRIPT_VERSIONED)
	$(EXECUTABLE_FILE) $(GHCII_SCRIPT_VERSIONED)

endif

