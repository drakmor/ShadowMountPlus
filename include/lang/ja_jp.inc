static const char *const g_ja_jp[SM_L10N_COUNT] = {
    /* English: Go to Debug Settings */
    [SM_L10N_ACTION_DEBUG_SETTINGS] = "デバッグ設定へ移動",
    /* English: ShadowMount+ v%s exFAT/UFS/PFS */
    [SM_L10N_STARTUP] = "ShadowMount+ v%s exFAT/UFS/PFS",
    /* English: ShadowMount+: recursive_scan=1 is deprecated, using scan_depth=2. */
    [SM_L10N_RECURSIVE_SCAN_DEPRECATED] = "ShadowMount+: recursive_scan=1 は非推奨です。scan_depth=2 を使用します。",
    /* English: ShadowMount+: scan depth %u enabled. */
    [SM_L10N_SCAN_DEPTH_ENABLED] = "ShadowMount+: スキャン深度 %u が有効です。",
    /* English: Backport overlay failed: %s\n%s\n0x%08X */
    [SM_L10N_BACKPORT_OVERLAY_FAILED] = "バックポートオーバーレイのマウントに失敗しました: %s\n%s\n0x%08X",
    /* English: Backport applied to game: %s */
    [SM_L10N_GAME_BACKPORTED] = "ゲームにバックポートを適用しました: %s",
    /* English: Backport applied to game: %s\nEmulators updated */
    [SM_L10N_GAME_BACKPORTED_EMULATORS_UPDATED] = "ゲームにバックポートを適用しました: %s\n複数のエミュレーターを更新しました",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nSaved image_sector override: %u.\nTry mounting again. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_AUTOTUNED] = "イメージのマウントが拒否されました:\n%s\nクラスタサイズが小さすぎます (%llu < %u)。\nimage_sector override を保存しました: %u。\nもう一度マウントしてください。",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nUse a larger cluster size in the image or lower sector size in config. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_CONFIG] = "イメージのマウントが拒否されました:\n%s\nクラスタサイズが小さすぎます (%llu < %u)。\nイメージのクラスタサイズを大きくするか、設定のセクタサイズを小さくしてください。",
    /* English: Register failed: %s (%s)\nAppInstallTitleDir unavailable */
    [SM_L10N_REGISTER_FAILED_UNAVAILABLE] = "登録に失敗しました: %s (%s)\nAppInstallTitleDir が利用できません",
    /* English: Register failed: %s (%s)\nTitleDir bridge unavailable */
    [SM_L10N_REGISTER_BRIDGE_UNAVAILABLE] = "登録に失敗しました: %s (%s)\nTitleDir ブリッジを利用できません",
    /* English: Register failed: %s (%s)\ncode=0x%08X */
    [SM_L10N_REGISTER_FAILED_CODE] = "登録に失敗しました: %s (%s)\nコード=0x%08X",
    /* English: Installing: %s (%s)... */
    [SM_L10N_INSTALLING] = "インストール中: %s (%s)...",
    /* English: KStuff active again */
    [SM_L10N_KSTUFF_ACTIVE] = "KStuff が再び有効です",
    /* English: KStuff paused while the game is running */
    [SM_L10N_KSTUFF_PAUSED] = "ゲーム実行中のため KStuff を一時停止しました",
    /* English: Installed: %s */
    [SM_L10N_INSTALLED] = "インストール済み: %s",
    /* English: Unknown */
    [SM_L10N_UNKNOWN] = "不明",
    /* English: Image mount failed: 0x%08X (%s)\n%s */
    [SM_L10N_IMAGE_MOUNT_FAILED] = "イメージのマウントに失敗しました: 0x%08X (%s)\n%s",
    /* English: Filesystem cluster size (%llu) is smaller than device sector size (%u) for %s */
    [SM_L10N_FS_CLUSTER_SMALL] = "ファイルシステムのクラスタサイズ (%llu) がデバイスセクタサイズ (%u) より小さいです: %s",
    /* English: Mounted image is unreadable or damaged */
    [SM_L10N_IMAGE_UNREADABLE_DAMAGED] = "マウント済みイメージを読み取れないか破損しています",
    /* English: Image cache full (%u entries), cannot track mount %s */
    [SM_L10N_IMAGE_CACHE_FULL_TRACK] = "イメージキャッシュが満杯です (%u 件)。マウント %s を追跡できません",
    /* English: Image cache full (%u entries), rolling back mount */
    [SM_L10N_IMAGE_CACHE_FULL_ROLLBACK] = "イメージキャッシュが満杯です (%u 件)。マウントを元に戻します",
    /* English: Install failed: %s (%s)\nTimed out after 5 minutes */
    [SM_L10N_INSTALL_FAILED_TIMEOUT] = "インストールに失敗しました: %s (%s)\n5 分後にタイムアウトしました",
    /* English: Batch install queued (%d): */
    [SM_L10N_BATCH_INSTALL_QUEUED] = "一括インストールをキューに追加しました (%d):",
    /* English: \n... and %d more */
    [SM_L10N_BATCH_INSTALL_AND_MORE] = "\n... 他 %d 件",
    /* English: Batch install failed for %d app(s).\ncode=0x%08X */
    [SM_L10N_BATCH_INSTALL_FAILED] = "%d 個のアプリの一括インストールに失敗しました。\nコード=0x%08X",
    /* English: %s crashed before KStuff was paused. KStuff is not the cause. */
    [SM_L10N_CRASH_BEFORE_KSTUFF] = "%s は KStuff の一時停止前にクラッシュしました。KStuff が原因ではありません。",
    /* English: %s crashed before KStuff pause:\n%s */
    [SM_L10N_CRASH_BEFORE_KSTUFF_FATAL] = "%s は KStuff の一時停止前にクラッシュしました:\n%s",
    /* English: %s crashed after KStuff was paused. Automatic tuning skipped. */
    [SM_L10N_CRASH_AFTER_KSTUFF_SKIPPED] = "%s は KStuff の一時停止後にクラッシュしました。自動調整をスキップしました。",
    /* English: %s crashed after KStuff pause:\n%s\nAutotune skipped. */
    [SM_L10N_CRASH_AFTER_KSTUFF_FATAL_SKIPPED] = "%s は KStuff の一時停止後にクラッシュしました:\n%s\n自動調整をスキップしました。",
    /* English: Can't load module after KStuff pause */
    [SM_L10N_RTLD_AFTER_KSTUFF_FALLBACK] = "KStuff 一時停止後にモジュールを読み込めません",
    /* English: can't load module %.*s after KStuff pause */
    [SM_L10N_RTLD_MODULE_AFTER_KSTUFF] = "KStuff の一時停止後にモジュール %.*s を読み込めません",
    /* English: %s: %s. Delay increased to %us.\nLaunch the game again. */
    [SM_L10N_DELAY_INCREASED_RELAUNCH] = "%s: %s。遅延を %u 秒に増やしました。\nゲームをもう一度起動してください。",
    /* English: Crash detected after KStuff pause: pause delay for %s increased to %us. Launch the game again. */
    [SM_L10N_CRASH_DELAY_INCREASED] = "KStuff の一時停止後にクラッシュを検出しました: %s の遅延を %u 秒に増やしました。ゲームをもう一度起動してください。",
    /* English: %s crashed after KStuff pause:\n%s\nDelay increased to %us.\nLaunch the game again. */
    [SM_L10N_CRASH_FATAL_DELAY_INCREASED] = "%s は KStuff の一時停止後にクラッシュしました:\n%s\n遅延を %u 秒に増やしました。\nゲームをもう一度起動してください。",
    /* English: Crash detected after KStuff pause: %s. Failed to update autotune delay. */
    [SM_L10N_CRASH_DELAY_UPDATE_FAILED] = "KStuff の一時停止後にクラッシュを検出しました: %s。自動調整の遅延を更新できませんでした。",
    /* English: %s crashed after KStuff pause:\n%s\nFailed to update autotune delay. */
    [SM_L10N_CRASH_FATAL_DELAY_UPDATE_FAILED] = "%s は KStuff の一時停止後にクラッシュしました:\n%s\n自動調整の遅延を更新できませんでした。",
    /* English: Missing/invalid param.json:\n%s */
    [SM_L10N_MISSING_PARAM] = "param.json が見つからないか無効です:\n%s",
    /* English: Found %d new games. Processing... */
    [SM_L10N_FOUND_NEW_GAMES] = "%d 本の新しいゲームを検出しました。処理中...",
    /* English: Library synchronized.\nFound %d games. */
    [SM_L10N_LIBRARY_SYNCED] = "ライブラリを同期しました。\n%d 本のゲームを検出しました。",
    /* English: ShadowMount+: config reloaded. */
    [SM_L10N_CONFIG_RELOADED] = "ShadowMount+: 設定を再読み込みしました。",
    /* English: Duplicate %s ignored:\n%s\nexisting: %s */
    [SM_L10N_DUPLICATE_IGNORED] = "重複 %s を無視しました:\n%s\n既存: %s",
    /* English: AMPR emulator updated */
    [SM_L10N_AMPR_EMULATOR_UPDATED] = "AMPRエミュレーターを更新しました",
    /* English: ShadowMount+: failed to install ShellCore hooks */
    [SM_L10N_SHELLCORE_HOOKS_FAILED] = "ShadowMount+: ShellCoreフックのインストールに失敗しました",
};
