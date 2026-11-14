static const char *const g_fi_fi[SM_L10N_COUNT] = {
    /* English: Go to Debug Settings */
    [SM_L10N_ACTION_DEBUG_SETTINGS] = "Siirry virheenkorjausasetuksiin",
    /* English: ShadowKraken v%s\n© Drakmor and Community */
    [SM_L10N_STARTUP] = "ShadowKraken v%s\n© Drakmor ja yhteisö",
    /* English: ShadowKraken v%s\n© Drakmor and Community\nControl: http://%s:%u */
    [SM_L10N_STARTUP_WEB] =
        "ShadowKraken v%s\n© Drakmor ja yhteisö\nHallinta: http://%s:%u",
    /* English: ShadowKraken: recursive_scan=1 is deprecated, using scan_depth=2. */
    [SM_L10N_RECURSIVE_SCAN_DEPRECATED] = "ShadowKraken: recursive_scan=1 on vanhentunut, käytetään scan_depth=2.",
    /* English: ShadowKraken: scan depth %u enabled. */
    [SM_L10N_SCAN_DEPTH_ENABLED] = "ShadowKraken: skannaussyvyys %u käytössä.",
    /* English: Backport overlay failed: %s\n%s\n0x%08X */
    [SM_L10N_BACKPORT_OVERLAY_FAILED] = "Backport-overlay epäonnistui: %s\n%s\n0x%08X",
    /* English: Emulators applied to game: %s */
    [SM_L10N_GAME_EMULATORS_APPLIED] = "Emulaattorit otettu käyttöön pelissä: %s",
    /* English: Backport applied: %s */
    [SM_L10N_GAME_BACKPORTED] = "Backport käytössä: %s",
    /* English: Backport applied: %s\nEmulators updated */
    [SM_L10N_GAME_BACKPORTED_EMULATORS_UPDATED] = "Backport käytössä: %s\nEmulaattorit päivitetty",
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
    /* English: Missing/invalid param.json/param.sfo:\n%s */
    [SM_L10N_MISSING_PARAM] = "param.json/param.sfo puuttuu tai on virheellinen:\n%s",
    /* English: Found %d new games. Processing... */
    [SM_L10N_FOUND_NEW_GAMES] = "Löytyi %d uutta peliä. Käsitellään...",
    /* English: Library synchronized.\nFound %d games. */
    [SM_L10N_LIBRARY_SYNCED] = "Kirjasto synkronoitu.\nLöytyi %d peliä.",
    /* English: ShadowKraken: config reloaded. */
    [SM_L10N_CONFIG_RELOADED] = "ShadowKraken: määritykset ladattiin uudelleen.",
    /* English: Duplicate %s ignored:\n%s\nexisting: %s */
    [SM_L10N_DUPLICATE_IGNORED] = "Kaksoiskappale %s ohitettu:\n%s\nolemassa: %s",
    /* English: AMPR emulator updated */
    [SM_L10N_AMPR_EMULATOR_UPDATED] = "AMPR-emulaattori päivitetty",
    /* English: ShadowKraken: failed to install ShellCore hooks */
    [SM_L10N_SHELLCORE_HOOKS_FAILED] = "ShadowKraken: ShellCore-hookkien asennus epäonnistui",
    /* English: ShadowKraken: kstuff not found; waiting for it to start. */
    [SM_L10N_KSTUFF_WAITING] = "ShadowKraken: kstuffia ei löytynyt; odotetaan sen käynnistymistä",
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
