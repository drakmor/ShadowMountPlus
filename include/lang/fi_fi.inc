static const char *const g_fi_fi[SM_L10N_COUNT] = {
    /* English: Go to Debug Settings */
    [SM_L10N_ACTION_DEBUG_SETTINGS] = "Siirry virheenkorjausasetuksiin",
    /* English: ShadowMount+ v%s exFAT/UFS/PFS */
    [SM_L10N_STARTUP] = "ShadowMount+ v%s exFAT/UFS/PFS",
    /* English: ShadowMount+: recursive_scan=1 is deprecated, using scan_depth=2. */
    [SM_L10N_RECURSIVE_SCAN_DEPRECATED] = "ShadowMount+: recursive_scan=1 on vanhentunut, käytetään scan_depth=2.",
    /* English: ShadowMount+: scan depth %u enabled. */
    [SM_L10N_SCAN_DEPTH_ENABLED] = "ShadowMount+: skannaussyvyys %u käytössä.",
    /* English: Backport overlay failed: %s\n%s\n0x%08X */
    [SM_L10N_BACKPORT_OVERLAY_FAILED] = "Backport-overlay epäonnistui: %s\n%s\n0x%08X",
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
    /* English: KStuff active again */
    [SM_L10N_KSTUFF_ACTIVE] = "KStuff on taas aktiivinen",
    /* English: KStuff paused while the game is running */
    [SM_L10N_KSTUFF_PAUSED] = "KStuff keskeytetty pelin ollessa käynnissä",
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
    /* English: Image cache full (%u entries), rolling back mount */
    [SM_L10N_IMAGE_CACHE_FULL_ROLLBACK] = "Levykuvavälimuisti on täynnä (%u merkintää), liittäminen perutaan",
    /* English: Install failed: %s (%s)\nTimed out after 5 minutes */
    [SM_L10N_INSTALL_FAILED_TIMEOUT] = "Asennus epäonnistui: %s (%s)\nAikakatkaisu 5 minuutin jälkeen",
    /* English: Batch install queued (%d): */
    [SM_L10N_BATCH_INSTALL_QUEUED] = "Eräasennus jonossa (%d):",
    /* English: \n... and %d more */
    [SM_L10N_BATCH_INSTALL_AND_MORE] = "\n... ja %d lisää",
    /* English: Batch install failed for %d app(s).\ncode=0x%08X */
    [SM_L10N_BATCH_INSTALL_FAILED] = "Eräasennus epäonnistui %d sovellukselle.\nkoodi=0x%08X",
    /* English: %s crashed before KStuff was paused. KStuff is not the cause. */
    [SM_L10N_CRASH_BEFORE_KSTUFF] = "Sovellus %s kaatui ennen KStuffin keskeytystä. KStuff ei ole syyllinen.",
    /* English: %s crashed before KStuff pause:\n%s */
    [SM_L10N_CRASH_BEFORE_KSTUFF_FATAL] = "%s kaatui ennen KStuffin keskeytystä:\n%s",
    /* English: %s crashed after KStuff was paused. Automatic tuning skipped. */
    [SM_L10N_CRASH_AFTER_KSTUFF_SKIPPED] = "Sovellus %s kaatui KStuffin keskeytyksen jälkeen. Automaattinen säätö ohitettiin.",
    /* English: %s crashed after KStuff pause:\n%s\nAutotune skipped. */
    [SM_L10N_CRASH_AFTER_KSTUFF_FATAL_SKIPPED] = "%s kaatui KStuffin keskeytyksen jälkeen:\n%s\nAutomaattinen säätö ohitettiin.",
    /* English: Can't load module after KStuff pause */
    [SM_L10N_RTLD_AFTER_KSTUFF_FALLBACK] = "Moduulia ei voi ladata KStuff-tauon jälkeen",
    /* English: can't load module %.*s after KStuff pause */
    [SM_L10N_RTLD_MODULE_AFTER_KSTUFF] = "moduulia %.*s ei voi ladata KStuffin keskeytyksen jälkeen",
    /* English: %s: %s. Delay increased to %us.\nLaunch the game again. */
    [SM_L10N_DELAY_INCREASED_RELAUNCH] = "%s: %s. Viive nostettu arvoon %u s.\nKäynnistä peli uudelleen.",
    /* English: Crash detected after KStuff pause: pause delay for %s increased to %us. Launch the game again. */
    [SM_L10N_CRASH_DELAY_INCREASED] = "KStuffin keskeytyksen jälkeen havaittiin kaatuminen: kohteen %s viive nostettiin arvoon %u s. Käynnistä peli uudelleen.",
    /* English: %s crashed after KStuff pause:\n%s\nDelay increased to %us.\nLaunch the game again. */
    [SM_L10N_CRASH_FATAL_DELAY_INCREASED] = "%s kaatui KStuffin keskeytyksen jälkeen:\n%s\nViive nostettiin arvoon %u s.\nKäynnistä peli uudelleen.",
    /* English: Crash detected after KStuff pause: %s. Failed to update autotune delay. */
    [SM_L10N_CRASH_DELAY_UPDATE_FAILED] = "KStuffin keskeytyksen jälkeen havaittiin kaatuminen: %s. Automaattisen säädön viivettä ei voitu päivittää.",
    /* English: %s crashed after KStuff pause:\n%s\nFailed to update autotune delay. */
    [SM_L10N_CRASH_FATAL_DELAY_UPDATE_FAILED] = "%s kaatui KStuffin keskeytyksen jälkeen:\n%s\nAutomaattisen säädön viivettä ei voitu päivittää.",
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
};
