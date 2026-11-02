/* SPDX-License-Identifier: Apache-2.0
 * 中文：升级元数据边界与版本兼容性。/ English: Release bounds and version compatibility. */
#include "ota_online.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static const char *valid = "{\"schema\":1,\"version\":\"0.3.3-rc79\",\"project\":\"" PICO_OTA_PROJECT "\",\"board\":\"" PICO_OTA_BOARD "\",\"layout\":\"" PICO_OTA_LAYOUT "\",\"minimum_base_version\":\"0.3.3-rc72\",\"url\":\"https://wegooo-cell.github.io/read-pico-reader/Pico-update-0.3.3-rc79.bin\",\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\",\"notes\":\"Test release\",\"size\":4096}";
int main(void) {
 pico_release_t release;
 assert(pico_release_parse(valid,strlen(valid),&release)==ESP_OK);
 assert(release.size==4096 && !strcmp(release.version,"0.3.3-rc79"));
 assert(pico_version_compare("0.3.3-rc79","0.3.3-rc9")>0);
 assert(pico_version_compare("0.3.3","0.3.3-rc79")>0);
 assert(pico_version_compare("0.4.0-rc1","0.3.3")>0);
 // rc 小修订：rc98 < rc98.1 < rc98.2 < rc99 < 正式版。/ rc sub-revisions.
 assert(pico_version_compare("0.3.3-rc98.1","0.3.3-rc98")>0);
 assert(pico_version_compare("0.3.3-rc98","0.3.3-rc98.1")<0);
 assert(pico_version_compare("0.3.3-rc98.2","0.3.3-rc98.1")>0);
 assert(pico_version_compare("0.3.3-rc98.1","0.3.3-rc99")<0);
 assert(pico_version_compare("0.3.3-rc99","0.3.3-rc98.9")>0);
 assert(pico_version_compare("0.3.3","0.3.3-rc98.1")>0);
 assert(pico_version_compare("0.3.3-rc98.1","0.3.3-rc98.1")==0);
 // 非法小修订仍被拒绝：解析失败时比较为 0，清单也拒收。/ Malformed ones stay rejected.
 assert(pico_version_compare("0.3.3-rc98.","0.3.3-rc98.1")==0);
 assert(pico_version_compare("0.3.3-rc98.1.2","0.3.3-rc98.1")==0);
 assert(pico_version_compare("0.3.3-rc98.1x","0.3.3-rc98.1")==0);
 assert(pico_version_compare("0.3.3-rc.1","0.3.3-rc98.1")==0);
 assert(pico_release_url_valid("https://kiikoread.com/Pico-update-0.3.3-rc90.bin"));
 assert(!pico_release_url_valid("https://kiikoread.com.evil/a.bin"));
 assert(!pico_release_url_valid("https://kiikoread.com@evil/a.bin"));
 assert(!pico_release_url_valid("https://kiikoread.com:8443/a.bin"));
 assert(!pico_release_url_valid("https://kiikoread.com/../a.bin"));
 assert(!pico_release_url_valid("https://kiikoread.com/a.bin?url=evil"));
 assert(!pico_release_url_valid("http://wegooo-cell.github.io/read-pico-reader/a.bin"));
 assert(!pico_release_url_valid("https://wegooo-cell.github.io.evil/read-pico-reader/a.bin"));
 assert(!pico_release_url_valid("https://wegooo-cell.github.io/read-pico-reader/../a.bin"));
 // Anki 版通道：只多一个白名单站点，扁平文件名与其余规则一律不变。
 // Anki channel: exactly one more allowlisted site; flat names and every other rule stand.
 assert(pico_release_url_valid("https://cyankirin.github.io/kiikoreader-anki/Pico-anki-0.3.3-rc104.02.bin"));
 assert(!pico_release_url_valid("https://cyankirin.github.io/kiikoreader-anki/releases/0.3.3-rc104.02/firmware.bin"));
 assert(!pico_release_url_valid("https://cyankirin.github.io/kiikoreader-anki/../Pico-anki.bin"));
 assert(!pico_release_url_valid("https://cyankirin.github.io/kiikoreader-anki/Pico-anki.tar"));
 assert(!pico_release_url_valid("https://cyankirin.github.io/other/Pico-anki.bin"));
 assert(!pico_release_url_valid("https://github.com/cyankirin/kiikoreader-anki/releases/download/x/y.bin"));
 assert(!pico_release_url_valid("http://cyankirin.github.io/kiikoreader-anki/Pico-anki.bin"));
 assert(strstr(PICO_OTA_ANKI_FEED_URL, "cyankirin.github.io/kiikoreader-anki/") != NULL);
 // 站点产出的 Anki 清单形状必须能被设备解析。/ The site's Anki manifest shape parses on the device.
 char anki[2048];
 snprintf(anki, sizeof(anki), "{\"schema\":1,\"version\":\"0.3.3-rc104.02\",\"project\":\"%s\",\"board\":\"%s\",\"layout\":\"%s\",\"minimum_base_version\":\"0.3.3-rc72\",\"url\":\"https://cyankirin.github.io/kiikoreader-anki/Pico-anki-0.3.3-rc104.02.bin\",\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\",\"notes\":\"Anki 版：导入 .apkg 复习\",\"size\":3632576}", PICO_OTA_PROJECT, PICO_OTA_BOARD, PICO_OTA_LAYOUT);
 assert(pico_release_parse(anki, strlen(anki), &release) == ESP_OK);
 assert(!strcmp(release.version, "0.3.3-rc104.02") && release.size == 3632576);
 // rc 小修订让同一个 rc 内的再次发布也能被发现。/ An rc sub-revision is still a newer release.
 assert(pico_version_compare("0.3.3-rc104.02", "0.3.3-rc104") > 0);
 assert(pico_version_compare("0.3.3-rc104.02", "0.3.3-rc104.02") == 0);
 assert(pico_version_compare("0.3.3-rc104.02", "0.3.3-rc104.01") > 0);
 assert(pico_release_parse(valid,strlen(valid)+1,&release)!=ESP_OK);
 assert(pico_release_parse("{}",2,&release)!=ESP_OK);
 char altered[2048]; snprintf(altered,sizeof(altered),"%s junk",valid);
 assert(pico_release_parse(altered,strlen(altered),&release)!=ESP_OK);
 snprintf(altered,sizeof(altered),"{\"schema\":1,%s",valid+1);
 assert(pico_release_parse(altered,strlen(altered),&release)!=ESP_OK);
 strcpy(altered,valid); char *p=strstr(altered,"4096"); memcpy(p,"1e10",4);
 assert(pico_release_parse(altered,strlen(altered),&release)!=ESP_OK);
 strcpy(altered,valid); p=strstr(altered,PICO_OTA_BOARD); p[0]='X';
 assert(pico_release_parse(altered,strlen(altered),&release)!=ESP_OK);
 strcpy(altered,valid); p=strstr(altered,"0123456789abcdef"); p[0]='z';
 assert(pico_release_parse(altered,strlen(altered),&release)!=ESP_OK);
 snprintf(altered,sizeof(altered),"%s",valid); p=strstr(altered,PICO_OTA_PROJECT);p[0]='X';
 assert(pico_release_parse(altered,strlen(altered),&release)!=ESP_OK);
 const unsigned sizes[]={0x400001,0x500000,0x500001};
 for(unsigned i=0;i<3;++i){p=strstr(valid,"4096");size_t prefix=p-valid;
  snprintf(altered,sizeof(altered),"%.*s%u%s",(int)prefix,valid,sizes[i],p+4);
  assert((pico_release_parse(altered,strlen(altered),&release)==ESP_OK)==(sizes[i]<=PICO_OTA_IMAGE_MAX));
 }
 // 清单层：rc 小修订可被接受，多一级后缀仍被拒。/ Manifest level: rc sub-revision accepted, deeper suffix rejected.
 const char *vpos = strstr(valid, "0.3.3-rc79");
 char spliced[2048];
 snprintf(spliced, sizeof(spliced), "%.*s0.3.3-rc98.1%s", (int)(vpos - valid), valid, vpos + 10);
 assert(pico_release_parse(spliced, strlen(spliced), &release) == ESP_OK);
 assert(!strcmp(release.version, "0.3.3-rc98.1"));
 snprintf(spliced, sizeof(spliced), "%.*s0.3.3-rc98.1.2%s", (int)(vpos - valid), valid, vpos + 10);
 assert(pico_release_parse(spliced, strlen(spliced), &release) != ESP_OK);
 puts("PASS: release schema, bounded size, hashes, version ordering, rc sub-revisions, board, duplicate/trailing payload and HTTPS origin");
}
