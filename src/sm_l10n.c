#include "sm_l10n.h"

#include "sm_config_mount.h"
#include "sm_log.h"
#include "sm_types.h"

#define SCE_SYSTEM_SERVICE_PARAM_ID_LANG 1

typedef struct {
  const char *country;
  const char *lang;
  const char *locale;
} sm_region_t;

static const sm_region_t g_regions[] = {
    {"jp", "ja", "ja-JP"}, {"us", "en", "en-US"},
    {"fr", "fr", "fr-FR"}, {"es", "es", "es-ES"},
    {"de", "de", "de-DE"}, {"it", "it", "it-IT"},
    {"nl", "nl", "nl-NL"}, {"pt", "pt", "pt-PT"},
    {"ru", "ru", "ru-RU"}, {"kr", "ko", "ko-KR"},
    {"tw", "zh", "zh-TW"}, {"cn", "zh", "zh-CN"},
    {"fi", "fi", "fi-FI"}, {"se", "sv", "sv-SE"},
    {"dk", "da", "da-DK"}, {"no", "no", "no-NO"},
    {"pl", "pl", "pl-PL"}, {"br", "pt", "pt-BR"},
    {"gb", "en", "en-GB"}, {"tr", "tr", "tr-TR"},
    {"mx", "es", "es-MX"}, {"sa", "ar", "ar-SA"},
    {"ca", "fr", "fr-CA"}, {"cz", "cs", "cs-CZ"},
    {"hu", "hu", "hu-HU"}, {"gr", "el", "el-GR"},
    {"ro", "ro", "ro-RO"}, {"th", "th", "th-TH"},
    {"vn", "vi", "vi-VN"}, {"id", "id", "id-ID"},
};

#include "lang/ar_sa.inc"
#include "lang/cs_cz.inc"
#include "lang/da_dk.inc"
#include "lang/de_de.inc"
#include "lang/el_gr.inc"
#include "lang/en_gb.inc"
#include "lang/en_us.inc"
#include "lang/es_es.inc"
#include "lang/es_mx.inc"
#include "lang/fi_fi.inc"
#include "lang/fr_ca.inc"
#include "lang/fr_fr.inc"
#include "lang/hu_hu.inc"
#include "lang/id_id.inc"
#include "lang/it_it.inc"
#include "lang/ja_jp.inc"
#include "lang/ko_kr.inc"
#include "lang/nl_nl.inc"
#include "lang/no_no.inc"
#include "lang/pl_pl.inc"
#include "lang/pt_br.inc"
#include "lang/pt_pt.inc"
#include "lang/ro_ro.inc"
#include "lang/ru_ru.inc"
#include "lang/sv_se.inc"
#include "lang/th_th.inc"
#include "lang/tr_tr.inc"
#include "lang/vi_vn.inc"
#include "lang/zh_cn.inc"
#include "lang/zh_tw.inc"

static int g_system_lang = 1;
static bool g_l10n_initialized = false;

static const char *const *active_catalog(void) {
  switch (g_system_lang) {
  case 0:
    return g_ja_jp;
  case 2:
    return g_fr_fr;
  case 3:
    return g_es_es;
  case 4:
    return g_de_de;
  case 5:
    return g_it_it;
  case 6:
    return g_nl_nl;
  case 7:
    return g_pt_pt;
  case 8:
    return g_ru_ru;
  case 9:
    return g_ko_kr;
  case 10:
    return g_zh_tw;
  case 11:
    return g_zh_cn;
  case 12:
    return g_fi_fi;
  case 13:
    return g_sv_se;
  case 14:
    return g_da_dk;
  case 15:
    return g_no_no;
  case 16:
    return g_pl_pl;
  case 17:
    return g_pt_br;
  case 18:
    return g_en_gb;
  case 19:
    return g_tr_tr;
  case 20:
    return g_es_mx;
  case 21:
    return g_ar_sa;
  case 22:
    return g_fr_ca;
  case 23:
    return g_cs_cz;
  case 24:
    return g_hu_hu;
  case 25:
    return g_el_gr;
  case 26:
    return g_ro_ro;
  case 27:
    return g_th_th;
  case 28:
    return g_vi_vn;
  case 29:
    return g_id_id;
  default:
    return g_en_us;
  }
}

void sm_l10n_init(void) {
  int32_t sys_lang = -1;
  if (sceSystemServiceParamGetInt(SCE_SYSTEM_SERVICE_PARAM_ID_LANG,
                                  &sys_lang) == 0 &&
      sys_lang >= 0 &&
      sys_lang < (int32_t)(sizeof(g_regions) / sizeof(g_regions[0]))) {
    g_system_lang = sys_lang;
  } else {
    g_system_lang = 1;
  }

  if (!g_l10n_initialized || runtime_config()->debug_enabled) {
    const sm_region_t *region = &g_regions[g_system_lang];
    log_debug("  [L10N] system language=%d country=%s lang=%s locale=%s",
              g_system_lang, region->country, region->lang, region->locale);
  }
  g_l10n_initialized = true;
}

const char *sm_l10n_get(sm_l10n_key_t key) {
  if (!g_l10n_initialized)
    sm_l10n_init();
  if (key < 0 || key >= SM_L10N_COUNT)
    return "";

  const char *const *catalog = active_catalog();
  if (catalog[key])
    return catalog[key];
  return g_en_us[key] ? g_en_us[key] : "";
}

const char *sm_l10n_lang(void) {
  if (!g_l10n_initialized)
    sm_l10n_init();
  return g_regions[g_system_lang].lang;
}

const char *sm_l10n_locale(void) {
  if (!g_l10n_initialized)
    sm_l10n_init();
  return g_regions[g_system_lang].locale;
}
