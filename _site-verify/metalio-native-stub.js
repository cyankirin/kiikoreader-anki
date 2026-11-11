/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：加载与本地 esptool 5.5.0 字节相同的 Espressif S31 RAM stub；先校验来源摘要，再写 RAM。
 * English: Load the byte-identical native esptool 5.5.0 Espressif S31 RAM stub, verifying its pinned digest before RAM upload.
 */
import {require} from './metalio-flash-core.js';
export const S31_STUB_SHA256='bccb338d09714d35283048936056adc2ba4d2172a694ad1f79a08ed04cc242ad';
export async function loadS31Stub(fetcher=fetch){
  const response=await fetcher(new URL('./vendor/metalio/esp32s31-esptool-5.5.0.json',import.meta.url),{cache:'no-store',signal:AbortSignal.timeout(10000)});
  require(response.ok,'下载 S31 官方 RAM stub 失败，尚未写入。');
  const bytes=new Uint8Array(await response.arrayBuffer());
  const digest=Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',bytes)),b=>b.toString(16).padStart(2,'0')).join('');
  require(digest===S31_STUB_SHA256,'S31 RAM stub 校验失败，尚未写入。');
  const json=JSON.parse(new TextDecoder().decode(bytes));
  require(json.text_start===788533248&&json.data_start===788647352&&json.entry===788538252,'S31 RAM stub 地址不符。');
  const decode=text=>Uint8Array.from(atob(text),c=>c.charCodeAt(0));
  return {...json,decodedText:decode(json.text),decodedData:decode(json.data),verified:true};
}
export async function runS31Stub(loader,stub){
  require(loader.chip?.CHIP_NAME==='ESP32-S31'&&loader.chip.IMAGE_CHIP_ID===32&&!loader.secureDownloadMode,'需要未加密的 S31 下载口。');
  require(!loader.syncStubDetected,'已运行的下载程序版本未确认，请重新进入 BOOT 后连接。');
  require(stub?.verified,'S31 RAM stub 尚未验证。');
  loader.info('上传与本地 esptool 5.5.0 相同的 Espressif S31 RAM stub…');
  const block=loader.ESP_RAM_BLOCK;
  for(const [address,bytes]of [[stub.text_start,stub.decodedText],[stub.data_start,stub.decodedData]]){
    if(!bytes.length)continue;
    await loader.memBegin(bytes.length,Math.ceil(bytes.length/block),block,address);
    for(let at=0,sequence=0;at<bytes.length;at+=block,sequence++)await loader.memBlock(bytes.subarray(at,at+block),sequence);
  }
  await loader.memFinish(stub.entry);
  const hello=await loader.transport.read(5000);
  require(hello.length===4&&hello.every((v,i)=>v===[79,72,65,73][i]),'S31 RAM stub 启动握手失败。');
  loader.IS_STUB=true;await loader.applyUsbFlashWriteSize();
  loader.info('Espressif S31 RAM stub 已运行（本地 esptool 5.5.0 同版）。');
  return loader.chip;
}
