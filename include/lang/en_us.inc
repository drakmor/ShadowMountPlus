static const char *const g_en_us[SM_L10N_COUNT] = {
    [SM_L10N_ACTION_DEBUG_SETTINGS] = "Go to Debug Settings",
    [SM_L10N_STARTUP] = "ShadowMount+ v%s exFAT/UFS/PFS",
    [SM_L10N_RECURSIVE_SCAN_DEPRECATED] =
        "ShadowMount+: recursive_scan=1 is deprecated, using scan_depth=2.",
    [SM_L10N_SCAN_DEPTH_ENABLED] = "ShadowMount+: scan depth %u enabled.",
    [SM_L10N_BACKPORT_OVERLAY_FAILED] =
        "Backport overlay failed: %s\n%s\n0x%08X",
    [SM_L10N_GAME_BACKPORTED] = "Game backported: %s",
    [SM_L10N_IMAGE_REJECTED_CLUSTER_AUTOTUNED] =
        "Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\n"
        "Saved image_sector override: %u.\nTry mounting again.",
    [SM_L10N_IMAGE_REJECTED_CLUSTER_CONFIG] =
        "Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\n"
        "Use a larger cluster size in the image or lower sector size in config.",
    [SM_L10N_REGISTER_FAILED_UNAVAILABLE] =
        "Register failed: %s (%s)\nAppInstallTitleDir unavailable",
    [SM_L10N_REGISTER_FAILED_CODE] = "Register failed: %s (%s)\ncode=0x%08X",
    [SM_L10N_INSTALLING] = "Installing: %s (%s)...",
    [SM_L10N_INSTALLED] = "Installed: %s",
    [SM_L10N_IMAGE_MOUNT_FAILED] = "Image mount failed: 0x%08X (%s)\n%s",
    [SM_L10N_FS_CLUSTER_SMALL] =
        "Filesystem cluster size (%llu) is smaller than device sector size "
        "(%u) for %s",
    [SM_L10N_IMAGE_UNREADABLE_DAMAGED] =
        "Mounted image is unreadable or damaged",
    [SM_L10N_IMAGE_CACHE_FULL_TRACK] =
        "Image cache full (%u entries), cannot track mount %s",
    [SM_L10N_IMAGE_CACHE_FULL_ROLLBACK] =
        "Image cache full (%u entries), rolling back mount",
    [SM_L10N_INSTALL_FAILED_TIMEOUT] =
        "Install failed: %s (%s)\nTimed out after 5 minutes",
    [SM_L10N_BATCH_INSTALL_QUEUED] = "Batch install queued (%d):",
    [SM_L10N_BATCH_INSTALL_AND_MORE] = "\n... and %d more",
    [SM_L10N_BATCH_INSTALL_FAILED] =
        "Batch install failed for %d app(s).\ncode=0x%08X",
    [SM_L10N_MISSING_PARAM] = "Missing/invalid param.json:\n%s",
    [SM_L10N_FOUND_NEW_GAMES] = "Found %d new games. Executing...",
    [SM_L10N_LIBRARY_SYNCED] = "Library Synchronized.\nFound %d games.",
    [SM_L10N_CONFIG_RELOADED] = "ShadowMount+: config reloaded.",
    [SM_L10N_DUPLICATE_IGNORED] =
        "Duplicate %s ignored:\n%s\nexisting: %s",
};
