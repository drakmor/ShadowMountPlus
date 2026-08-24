static const char *const g_ko_kr[SM_L10N_COUNT] = {
    /* English: Go to Debug Settings */
    [SM_L10N_ACTION_DEBUG_SETTINGS] = "디버그 설정으로 이동",
    /* English: ShadowMount+ v%s exFAT/UFS/PFS */
    [SM_L10N_STARTUP] = "ShadowMount+ v%s exFAT/UFS/PFS",
    /* English: ShadowMount+: recursive_scan=1 is deprecated, using scan_depth=2. */
    [SM_L10N_RECURSIVE_SCAN_DEPRECATED] = "ShadowMount+: recursive_scan=1은 더 이상 권장되지 않으며 scan_depth=2를 사용합니다.",
    /* English: ShadowMount+: scan depth %u enabled. */
    [SM_L10N_SCAN_DEPTH_ENABLED] = "ShadowMount+: 스캔 깊이 %u 활성화됨.",
    /* English: Backport overlay failed: %s\n%s\n0x%08X */
    [SM_L10N_BACKPORT_OVERLAY_FAILED] = "백포트 오버레이 마운트 실패: %s\n%s\n0x%08X",
    /* English: Backport applied to game: %s */
    [SM_L10N_GAME_BACKPORTED] = "게임에 백포트 적용됨: %s",
    /* English: Backport applied to game: %s\nEmulators updated */
    [SM_L10N_GAME_BACKPORTED_EMULATORS_UPDATED] = "게임에 백포트 적용됨: %s\n에뮬레이터들이 업데이트됨",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nSaved image_sector override: %u.\nTry mounting again. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_AUTOTUNED] = "이미지 마운트 거부됨:\n%s\n클러스터 크기가 너무 작습니다 (%llu < %u).\nimage_sector override 저장됨: %u.\n다시 마운트해 보세요.",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nUse a larger cluster size in the image or lower sector size in config. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_CONFIG] = "이미지 마운트가 거부되었습니다:\n%s\n클러스터 크기가 너무 작습니다(%llu < %u).\n이미지의 클러스터 크기를 늘리거나 설정의 섹터 크기를 줄이세요.",
    /* English: Register failed: %s (%s)\nAppInstallTitleDir unavailable */
    [SM_L10N_REGISTER_FAILED_UNAVAILABLE] = "등록 실패: %s (%s)\nAppInstallTitleDir 사용 불가",
    /* English: Register failed: %s (%s)\nTitleDir bridge unavailable */
    [SM_L10N_REGISTER_BRIDGE_UNAVAILABLE] = "등록 실패: %s (%s)\nTitleDir 브리지를 사용할 수 없음",
    /* English: Register failed: %s (%s)\ncode=0x%08X */
    [SM_L10N_REGISTER_FAILED_CODE] = "등록 실패: %s (%s)\n코드=0x%08X",
    /* English: Installing: %s (%s)... */
    [SM_L10N_INSTALLING] = "설치 중: %s (%s)...",
    /* English: KStuff active again */
    [SM_L10N_KSTUFF_ACTIVE] = "KStuff 다시 활성화됨",
    /* English: KStuff paused while the game is running */
    [SM_L10N_KSTUFF_PAUSED] = "게임 실행 중 KStuff 일시 중지됨",
    /* English: Installed: %s */
    [SM_L10N_INSTALLED] = "설치됨: %s",
    /* English: Unknown */
    [SM_L10N_UNKNOWN] = "알 수 없음",
    /* English: Image mount failed: 0x%08X (%s)\n%s */
    [SM_L10N_IMAGE_MOUNT_FAILED] = "이미지 마운트 실패: 0x%08X (%s)\n%s",
    /* English: Filesystem cluster size (%llu) is smaller than device sector size (%u) for %s */
    [SM_L10N_FS_CLUSTER_SMALL] = "파일 시스템 클러스터 크기(%llu)가 장치 섹터 크기(%u)보다 작습니다: %s",
    /* English: Mounted image is unreadable or damaged */
    [SM_L10N_IMAGE_UNREADABLE_DAMAGED] = "마운트된 이미지를 읽을 수 없거나 손상되었습니다",
    /* English: Image cache full (%u entries), cannot track mount %s */
    [SM_L10N_IMAGE_CACHE_FULL_TRACK] = "이미지 캐시가 가득 찼습니다(%u개). 마운트 %s을(를) 추적할 수 없습니다",
    /* English: Image cache full (%u entries), rolling back mount */
    [SM_L10N_IMAGE_CACHE_FULL_ROLLBACK] = "이미지 캐시가 가득 찼습니다(%u개). 마운트를 되돌립니다",
    /* English: Install failed: %s (%s)\nTimed out after 5 minutes */
    [SM_L10N_INSTALL_FAILED_TIMEOUT] = "설치 실패: %s (%s)\n5분 후 시간 초과",
    /* English: Batch install queued (%d): */
    [SM_L10N_BATCH_INSTALL_QUEUED] = "일괄 설치 대기열 추가됨 (%d):",
    /* English: \n... and %d more */
    [SM_L10N_BATCH_INSTALL_AND_MORE] = "\n... 외 %d개",
    /* English: Batch install failed for %d app(s).\ncode=0x%08X */
    [SM_L10N_BATCH_INSTALL_FAILED] = "%d개 앱 일괄 설치 실패.\n코드=0x%08X",
    /* English: %s crashed before KStuff was paused. KStuff is not the cause. */
    [SM_L10N_CRASH_BEFORE_KSTUFF] = "%s이(가) KStuff 일시 중지 전에 충돌했습니다. KStuff가 원인이 아닙니다.",
    /* English: %s crashed before KStuff pause:\n%s */
    [SM_L10N_CRASH_BEFORE_KSTUFF_FATAL] = "%s이(가) KStuff 일시 중지 전에 충돌했습니다:\n%s",
    /* English: %s crashed after KStuff was paused. Automatic tuning skipped. */
    [SM_L10N_CRASH_AFTER_KSTUFF_SKIPPED] = "%s이(가) KStuff 일시 중지 후에 충돌했습니다. 자동 조정을 건너뛰었습니다.",
    /* English: %s crashed after KStuff pause:\n%s\nAutotune skipped. */
    [SM_L10N_CRASH_AFTER_KSTUFF_FATAL_SKIPPED] = "%s이(가) KStuff 일시 중지 후에 충돌했습니다:\n%s\n자동 조정을 건너뛰었습니다.",
    /* English: Can't load module after KStuff pause */
    [SM_L10N_RTLD_AFTER_KSTUFF_FALLBACK] = "KStuff 일시 중지 후 모듈을 로드할 수 없습니다",
    /* English: can't load module %.*s after KStuff pause */
    [SM_L10N_RTLD_MODULE_AFTER_KSTUFF] = "KStuff 일시 중지 후 모듈 %.*s을(를) 로드할 수 없음",
    /* English: %s: %s. Delay increased to %us.\nLaunch the game again. */
    [SM_L10N_DELAY_INCREASED_RELAUNCH] = "%s: %s. 지연이 %u초로 증가했습니다.\n게임을 다시 실행하세요.",
    /* English: Crash detected after KStuff pause: pause delay for %s increased to %us. Launch the game again. */
    [SM_L10N_CRASH_DELAY_INCREASED] = "KStuff 일시 중지 후 충돌을 감지했습니다: %s의 지연을 %u초로 늘렸습니다. 게임을 다시 실행하세요.",
    /* English: %s crashed after KStuff pause:\n%s\nDelay increased to %us.\nLaunch the game again. */
    [SM_L10N_CRASH_FATAL_DELAY_INCREASED] = "%s이(가) KStuff 일시 중지 후에 충돌했습니다:\n%s\n지연을 %u초로 늘렸습니다.\n게임을 다시 실행하세요.",
    /* English: Crash detected after KStuff pause: %s. Failed to update autotune delay. */
    [SM_L10N_CRASH_DELAY_UPDATE_FAILED] = "KStuff 일시 중지 후 충돌을 감지했습니다: %s. 자동 조정 지연을 업데이트하지 못했습니다.",
    /* English: %s crashed after KStuff pause:\n%s\nFailed to update autotune delay. */
    [SM_L10N_CRASH_FATAL_DELAY_UPDATE_FAILED] = "%s이(가) KStuff 일시 중지 후에 충돌했습니다:\n%s\n자동 조정 지연을 업데이트하지 못했습니다.",
    /* English: Missing/invalid param.json:\n%s */
    [SM_L10N_MISSING_PARAM] = "param.json 누락 또는 잘못됨:\n%s",
    /* English: Found %d new games. Processing... */
    [SM_L10N_FOUND_NEW_GAMES] = "새 게임 %d개를 찾았습니다. 처리 중...",
    /* English: Library synchronized.\nFound %d games. */
    [SM_L10N_LIBRARY_SYNCED] = "라이브러리를 동기화했습니다.\n게임 %d개를 찾았습니다.",
    /* English: ShadowMount+: config reloaded. */
    [SM_L10N_CONFIG_RELOADED] = "ShadowMount+: 설정을 다시 불러왔습니다.",
    /* English: Duplicate %s ignored:\n%s\nexisting: %s */
    [SM_L10N_DUPLICATE_IGNORED] = "중복 %s 무시됨:\n%s\n기존: %s",
    /* English: AMPR emulator updated */
    [SM_L10N_AMPR_EMULATOR_UPDATED] = "AMPR 에뮬레이터가 업데이트되었습니다",
    /* English: ShadowMount+: failed to install ShellCore hooks */
    [SM_L10N_SHELLCORE_HOOKS_FAILED] = "ShadowMount+: ShellCore 후크 설치에 실패했습니다",
};
