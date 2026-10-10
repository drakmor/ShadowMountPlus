static const char *const g_zh_cn[SM_L10N_COUNT] = {
    /* English: Go to Debug Settings */
    [SM_L10N_ACTION_DEBUG_SETTINGS] = "前往调试设置",
    /* English: ShadowMount+ v%s\n© Drakmor and Community */
    [SM_L10N_STARTUP] = "ShadowMount+ v%s\n© Drakmor 与社区",
    /* English: ShadowMount+ v%s\n© Drakmor and Community\nControl: http://%s:%u */
    [SM_L10N_STARTUP_WEB] =
        "ShadowMount+ v%s\n© Drakmor 与社区\n管理: http://%s:%u",
    /* English: ShadowMount+: recursive_scan=1 is deprecated, using scan_depth=2. */
    [SM_L10N_RECURSIVE_SCAN_DEPRECATED] = "ShadowMount+: recursive_scan=1 已弃用，将使用 scan_depth=2。",
    /* English: ShadowMount+: scan depth %u enabled. */
    [SM_L10N_SCAN_DEPTH_ENABLED] = "ShadowMount+: 已启用扫描深度 %u。",
    /* English: Backport overlay failed: %s\n%s\n0x%08X */
    [SM_L10N_BACKPORT_OVERLAY_FAILED] = "向下兼容覆盖层挂载失败：%s\n%s\n0x%08X",
    /* English: Backport applied to game: %s */
    [SM_L10N_GAME_BACKPORTED] = "已为游戏应用向下兼容补丁：%s",
    /* English: Backport applied to game: %s\nEmulators updated */
    [SM_L10N_GAME_BACKPORTED_EMULATORS_UPDATED] = "已为游戏应用向下兼容补丁：%s\n多个模拟器已更新",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nSaved image_sector override: %u.\nTry mounting again. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_AUTOTUNED] = "镜像挂载被拒绝:\n%s\n簇大小太小 (%llu < %u)。\n已保存 image_sector 覆盖值: %u。\n请再次尝试挂载。",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nUse a larger cluster size in the image or lower sector size in config. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_CONFIG] = "镜像挂载被拒绝：\n%s\n簇大小过小（%llu < %u）。\n请增大镜像的簇大小，或在配置中减小扇区大小。",
    /* English: Register failed: %s (%s)\nAppInstallTitleDir unavailable */
    [SM_L10N_REGISTER_FAILED_UNAVAILABLE] = "注册失败: %s (%s)\nAppInstallTitleDir 不可用",
    /* English: Register failed: %s (%s)\nTitleDir bridge unavailable */
    [SM_L10N_REGISTER_BRIDGE_UNAVAILABLE] = "注册失败：%s（%s）\nTitleDir 桥接不可用",
    /* English: Register failed: %s (%s)\ncode=0x%08X */
    [SM_L10N_REGISTER_FAILED_CODE] = "注册失败: %s (%s)\n代码=0x%08X",
    /* English: Installing: %s (%s)... */
    [SM_L10N_INSTALLING] = "正在安装: %s (%s)...",
    /* English: Installed: %s */
    [SM_L10N_INSTALLED] = "已安装: %s",
    /* English: Unknown */
    [SM_L10N_UNKNOWN] = "未知",
    /* English: Image mount failed: 0x%08X (%s)\n%s */
    [SM_L10N_IMAGE_MOUNT_FAILED] = "镜像挂载失败: 0x%08X (%s)\n%s",
    /* English: Filesystem cluster size (%llu) is smaller than device sector size (%u) for %s */
    [SM_L10N_FS_CLUSTER_SMALL] = "文件系统簇大小 (%llu) 小于设备扇区大小 (%u)：%s",
    /* English: Mounted image is unreadable or damaged */
    [SM_L10N_IMAGE_UNREADABLE_DAMAGED] = "已挂载的镜像不可读或已损坏",
    /* English: Image cache full (%u entries), cannot track mount %s */
    [SM_L10N_IMAGE_CACHE_FULL_TRACK] = "镜像缓存已满（%u 项），无法跟踪挂载 %s",
    /* English: Install failed: %s (%s)\nTimed out after 5 minutes */
    [SM_L10N_INSTALL_FAILED_TIMEOUT] = "安装失败: %s (%s)\n5 分钟后超时",
    /* English: Batch install queued (%d): */
    [SM_L10N_BATCH_INSTALL_QUEUED] = "批量安装已排队 (%d):",
    /* English: \n... and %d more */
    [SM_L10N_BATCH_INSTALL_AND_MORE] = "\n... 以及另外 %d 个",
    /* English: Batch install failed for %d app(s).\ncode=0x%08X */
    [SM_L10N_BATCH_INSTALL_FAILED] = "%d 个应用批量安装失败。\n代码=0x%08X",
    /* English: Missing/invalid param.json:\n%s */
    [SM_L10N_MISSING_PARAM] = "param.json 缺失或无效:\n%s",
    /* English: Found %d new games. Processing... */
    [SM_L10N_FOUND_NEW_GAMES] = "发现 %d 个新游戏。正在处理...",
    /* English: Library synchronized.\nFound %d games. */
    [SM_L10N_LIBRARY_SYNCED] = "游戏库已同步。\n共发现 %d 个游戏。",
    /* English: ShadowMount+: config reloaded. */
    [SM_L10N_CONFIG_RELOADED] = "ShadowMount+：配置已重新加载。",
    /* English: Duplicate %s ignored:\n%s\nexisting: %s */
    [SM_L10N_DUPLICATE_IGNORED] = "已忽略重复项 %s：\n%s\n现有项：%s",
    /* English: AMPR emulator updated */
    [SM_L10N_AMPR_EMULATOR_UPDATED] = "AMPR 模拟器已更新",
    /* English: ShadowMount+: failed to install ShellCore hooks */
    [SM_L10N_SHELLCORE_HOOKS_FAILED] = "ShadowMount+：无法安装 ShellCore 钩子",
    /* English: Target temperature %u °C → %u °C */
    [SM_L10N_TEMPERATURE_TRANSITION] = "目标温度 %u °C → %u °C",
    /* English: External drive %s connected\nFree %llu.%llu GB of %llu.%llu (Block size %llu KB)\nGames on drive: %d%s */
    [SM_L10N_USB_CONNECTED_SCANNING] = "外部驱动器 %s 已连接。\n可用：%llu.%llu / %llu.%llu GB（块大小 %llu KB）\n驱动器上的游戏：%d%s",
    /* English: \nFor maximum performance, use a cluster size of no more than 64 KB */
    [SM_L10N_USB_CLUSTER_RECOMMENDATION] = "\n为获得最佳性能，请使用不超过 64 KB 的簇大小",
    /* English: External drive %s disconnected */
    [SM_L10N_USB_DISCONNECTED] = "外部驱动器 %s 已断开",
    /* English: Game %s crashed. */
    [SM_L10N_GAME_CRASHED] = "游戏 %s 崩溃了。",
    /* English: Error in game %s:\n%s */
    [SM_L10N_GAME_ERROR] = "游戏 %s 出错:\n%s",
};
