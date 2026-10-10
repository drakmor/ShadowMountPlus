static const char *const g_fi_fi[SM_L10N_COUNT] = {
    /* English: Go to Debug Settings */
    [SM_L10N_ACTION_DEBUG_SETTINGS] = "Siirry virheenkorjausasetuksiin",
    /* English: ShadowMount+ v%s\n© Drakmor and Community */
    [SM_L10N_STARTUP] = "ShadowMount+ v%s\n© Drakmor ja yhteisö",
    /* English: ShadowMount+ v%s\n© Drakmor and Community\nControl: http://%s:%u */
    [SM_L10N_STARTUP_WEB] =
        "ShadowMount+ v%s\n© Drakmor ja yhteisö\nHallinta: http://%s:%u",
    /* English: ShadowMount+: recursive_scan=1 is deprecated, using scan_depth=2. */
    [SM_L10N_RECURSIVE_SCAN_DEPRECATED] = "ShadowMount+: recursive_scan=1 on vanhentunut, käytetään scan_depth=2.",
    /* English: ShadowMount+: scan depth %u enabled. */
    [SM_L10N_SCAN_DEPTH_ENABLED] = "ShadowMount+: skannaussyvyys %u käytössä.",
    /* English: Backport overlay failed: %s\n%s\n0x%08X */
    [SM_L10N_BACKPORT_OVERLAY_FAILED] = "Backport-overlay epäonnistui: %s\n%s\n0x%08X",
    /* English: Emulators applied to game: %s */
    [SM_L10N_GAME_EMULATORS_APPLIED] = "Emulaattorit otettu käyttöön pelissä: %s",
    /* English: Backport applied to game: %s */
    [SM_L10N_GAME_BACKPORTED] = "Peliin käytettiin backportia: %s",
    /* English: Backport applied to game: %s\nEmulators updated */
    [SM_L10N_GAME_BACKPORTED_EMULATORS_UPDATED] = "Peliin käytettiin backportia: %s\nEmulaattorit päivitetty",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nSaved image_sector override: %u.\nTry mounting again. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_AUTOTUNED] = "Imagen liittäminen hylätty:\n%s\nKlusterikoko on liian pieni (%llu < %u).\nimage_sector-ohitus tallennettu: %u.\nYritä liittää uudelleen.",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nUse a larger cluster size in the image or lower sector size in config. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_CONFIG] = "Levykuvan liittäminen hylättiin:\n%s\nKlusterikoko on liian pieni (%llu < %u).\nKäytä levykuvassa suurempaa klusteria tai määrityksissä pienempää sektorikokoa.",
    /* English: Register failed: %s (%s)\nAppInstallTitleDir unavailable */
    [SM_L10N_REGISTER_FAILED_UNAVAILABLE] = "Rekisteröinti epäonnistui: %s (%s)\nAppInstallTitleDir ei ole käytettävissä",
    /* English: Register failed: %s (%s)\nTitleDir bridge unavailable */
    [SM_L10N_REGISTER_BRIDGE_UNAVAILABLE] = "Rekisteröinti epäonnistui: %s (%s)\nTitleDir-silta ei ole käytettävissä",
    /* English: Register failed: %s (%s)\ncode=0x%08X */
    [SM_L10N_REGISTER_FAILED_CODE] = "Rekisteröinti epäonnistui: %s (%s)\nkoodi=0x%08X",
    /* English: Installing: %s (%s)... */
    [SM_L10N_INSTALLING] = "Asennetaan: %s (%s)...",
    /* English: Installed: %s */
    [SM_L10N_INSTALLED] = "Asennettu: %s",
    /* English: Unknown */
    [SM_L10N_UNKNOWN] = "Tuntematon",
    /* English: Image mount failed: 0x%08X (%s)\n%s */
    [SM_L10N_IMAGE_MOUNT_FAILED] = "Imagen liittäminen epäonnistui: 0x%08X (%s)\n%s",
    /* English: Filesystem cluster size (%llu) is smaller than device sector size (%u) for %s */
    [SM_L10N_FS_CLUSTER_SMALL] = "Tiedostojärjestelmän klusterikoko (%llu) on pienempi kuin laitteen sektorikoko (%u) kohteelle %s",
    /* English: Mounted image is unreadable or damaged */
    [SM_L10N_IMAGE_UNREADABLE_DAMAGED] = "Liitetty levykuva ei ole luettavissa tai se on vioittunut",
    /* English: Image cache full (%u entries), cannot track mount %s */
    [SM_L10N_IMAGE_CACHE_FULL_TRACK] = "Levykuvavälimuisti on täynnä (%u merkintää), liittämistä %s ei voi seurata",
    /* English: Install failed: %s (%s)\nTimed out after 5 minutes */
    [SM_L10N_INSTALL_FAILED_TIMEOUT] = "Asennus epäonnistui: %s (%s)\nAikakatkaisu 5 minuutin jälkeen",
    /* English: Batch install queued (%d): */
    [SM_L10N_BATCH_INSTALL_QUEUED] = "Eräasennus jonossa (%d):",
    /* English: \n... and %d more */
    [SM_L10N_BATCH_INSTALL_AND_MORE] = "\n... ja %d lisää",
    /* English: Batch install failed for %d app(s).\ncode=0x%08X */
    [SM_L10N_BATCH_INSTALL_FAILED] = "Eräasennus epäonnistui %d sovellukselle.\nkoodi=0x%08X",
    /* English: Missing/invalid param.json:\n%s */
    [SM_L10N_MISSING_PARAM] = "param.json puuttuu tai on virheellinen:\n%s",
    /* English: Found %d new games. Processing... */
    [SM_L10N_FOUND_NEW_GAMES] = "Löytyi %d uutta peliä. Käsitellään...",
    /* English: Library synchronized.\nFound %d games. */
    [SM_L10N_LIBRARY_SYNCED] = "Kirjasto synkronoitu.\nLöytyi %d peliä.",
    /* English: ShadowMount+: config reloaded. */
    [SM_L10N_CONFIG_RELOADED] = "ShadowMount+: määritykset ladattiin uudelleen.",
    /* English: Duplicate %s ignored:\n%s\nexisting: %s */
    [SM_L10N_DUPLICATE_IGNORED] = "Kaksoiskappale %s ohitettu:\n%s\nolemassa: %s",
    /* English: AMPR emulator updated */
    [SM_L10N_AMPR_EMULATOR_UPDATED] = "AMPR-emulaattori päivitetty",
    /* English: ShadowMount+: failed to install ShellCore hooks */
    [SM_L10N_SHELLCORE_HOOKS_FAILED] = "ShadowMount+: ShellCore-hookkien asennus epäonnistui",
    /* English: Target temperature %u °C → %u °C */
    [SM_L10N_TEMPERATURE_TRANSITION] = "Tavoitelämpötila %u °C → %u °C",
    /* English: External drive %s connected\nFree %llu.%llu GB of %llu.%llu (Block size %llu KB)\nGames on drive: %d%s */
    [SM_L10N_USB_CONNECTED_SCANNING] = "Ulkoinen asema %s yhdistetty.\nVapaana: %llu.%llu / %llu.%llu Gt (lohko %llu kt)\nPelejä asemalla: %d%s",
    /* English: \nFor maximum performance, use a cluster size of no more than 64 KB */
    [SM_L10N_USB_CLUSTER_RECOMMENDATION] = "\nSaat parhaan suorituskyvyn käyttämällä enintään 64 kt:n klusterikokoa",
    /* English: External drive %s disconnected */
    [SM_L10N_USB_DISCONNECTED] = "Ulkoinen asema %s irrotettu",
    /* English: Game %s crashed. */
    [SM_L10N_GAME_CRASHED] = "Peli %s kaatui.",
    /* English: Error in game %s:\n%s */
    [SM_L10N_GAME_ERROR] = "Virhe pelissä %s:\n%s",
    /* English: External drive %s connected */
    [SM_L10N_USB_CONNECTED] = "Ulkoinen asema %s yhdistetty.",
};
