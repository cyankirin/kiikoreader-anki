/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：按 Espressif READ_FLASH 协议限制在途包，确认每包并收齐 MD5 结束帧；不修改 Flash。
 * English: Bound in-flight Espressif READ_FLASH frames, acknowledge each and validate the final MD5 frame; never modify Flash.
 */
import {require} from './metalio-flash-core.js';
import SparkMD5 from './vendor/metalio/spark-md5-3.0.2.js';
const failed=new WeakSet();
const word=value=>{const data=new Uint8Array(4);new DataView(data.buffer).setUint32(0,value,true);return data;};
const hex=data=>Array.from(data,b=>b.toString(16).padStart(2,'0')).join('');

export function flashDataMD5(data){return SparkMD5.ArrayBuffer.hash(data.buffer.slice(data.byteOffset,data.byteOffset+data.byteLength));}
export async function verifyS31Flash(loader,address,data){
  require(loader.chip?.IMAGE_CHIP_ID===32&&loader.IS_STUB,'需要已识别的 S31 下载程序。');
  require(!failed.has(loader),'读取会话已中断，请重新连接。');
  require(data instanceof Uint8Array&&data.length>0&&Number.isSafeInteger(address)&&address>=0&&address+data.length<=0x1000000,'Flash 校验范围无效。');
  const actual=await loader.flashMd5sum(address,data.length);
  require(typeof actual==='string'&&/^[0-9a-f]{32}$/i.test(actual)&&actual.toLowerCase()===flashDataMD5(data),'设备端 MD5 校验失败，不能确认写入成功。');
  return actual.toLowerCase();
}

export async function readS31Flash(loader,address,size,onProgress=()=>{},timeoutMs=Math.max(15000,Math.ceil(size/65536)*1000)){
  require(loader.chip?.IMAGE_CHIP_ID===32&&loader.IS_STUB,'需要已识别的 S31 下载程序。');
  require(!failed.has(loader),'读取会话已中断，请关闭页面并重新连接。');
  require(Number.isSafeInteger(address)&&Number.isSafeInteger(size)&&address>=0&&size>0&&address+size<=0x1000000,'读取范围超出 S31 Flash。');
  const data=new Uint8Array(size),hash=new SparkMD5.ArrayBuffer();
  // 浏览器串口分块与宿主串口不同；只允许一个小包在途，确认后再让 stub 继续。
  // WebSerial chunking differs from host serial; admit one small frame until its cumulative ACK.
  const packetSize=512,windowSize=1;
  const request=new Uint8Array(16),view=new DataView(request.buffer);
  [address,size,packetSize,windowSize].forEach((value,index)=>view.setUint32(index*4,value,true));
  let timer,received=0,phase='等待读取命令确认',stopped=false;
  const running=()=>require(!stopped,'读取已取消。');
  async function packet(){
    // SLIP 可带空分隔帧；有界跳过，不把它们当成 Flash 数据或校验结果。
    // SLIP may contain empty delimiter frames; skip a bounded count without accepting data/digests.
    for(let empty=0;empty<8;empty++){
      running();const next=await loader.transport.read(5000);running();
      if(next instanceof Uint8Array&&next.length===0)continue;
      return next;
    }
    throw Error('收到过多空数据帧。');
  }
  try{
    await Promise.race([
      (async()=>{
        require(await loader.checkCommand('read flash',loader.ESP_READ_FLASH,request,undefined,undefined,5000)===0,'S31 拒绝读取 Flash。');
        running();
        while(received<size){
          phase='等待数据包';const bytes=await packet();
          require(bytes instanceof Uint8Array&&bytes.length===Math.min(packetSize,size-received),'Flash 数据包长度异常，请重新连接。');
          data.set(bytes,received);hash.append(bytes.slice().buffer);received+=bytes.length;
          phase='发送接收确认';running();await loader.transport.write(word(received));running();onProgress(received,size);
        }
        // 必须等待结束帧，才允许下一条命令使用同一串口。
        // Consume the final frame before issuing another command on this transport.
        phase='等待 MD5 结束包';const digest=await packet();
        require(digest instanceof Uint8Array&&digest.length===16,'Flash 读取缺少结束校验包。');
        require(hash.end()===hex(digest),'Flash 读回数据 MD5 不匹配，已停止。');
      })(),
      new Promise((_,reject)=>{timer=setTimeout(()=>{stopped=true;reject(Error('Flash 读取超时。'));},timeoutMs);})
    ]);
    return data;
  }catch(error){
    stopped=true;failed.add(loader);
    const buffered=typeof loader.transport.inWaiting==='function'?loader.transport.inWaiting():'未知';
    throw Error(`Flash 读取中断：${received}/${size} 字节，${phase}，接收缓冲 ${buffered} 字节。${error.message} 已停止，请重新进入 BOOT 后连接。`,{cause:error});
  }finally{clearTimeout(timer);hash.destroy();}
}
