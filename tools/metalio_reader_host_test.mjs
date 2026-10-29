/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：回归 S31 读取帧流控、结束帧校验、连续命令、截断与超时保护。
 * English: Regress S31 read-frame flow control, final-digest validation, consecutive commands, truncation and timeouts.
 */
import assert from 'node:assert/strict';import {createHash,randomBytes} from 'node:crypto';import fs from 'node:fs';
import {readS31Flash,verifyS31Flash,flashDataMD5} from '../flash/metalio-flash-reader.js';
import {ESPLoader,Transport} from '../flash/vendor/metalio/esptool-js-0.7.0.js';
import {S31Transport} from '../flash/metalio-transport.js';
function fixture(data,options={}){
  let packets=[],received=0,commands=0,digests=0;
  const loader={chip:{IMAGE_CHIP_ID:32},IS_STUB:true,ESP_READ_FLASH:0xd2,
    async checkCommand(_label,op,request){
      assert.equal(packets.length,0,'previous final digest must be consumed before a new command');assert.equal(op,0xd2);
      const v=new DataView(request.buffer,request.byteOffset,request.byteLength);assert.equal(v.getUint32(4,true),data.length);assert.equal(v.getUint32(8,true),512);assert.equal(v.getUint32(12,true),1);
      commands++;received=0;
      for(let at=0;at<data.length;at+=512)packets.push(new Uint8Array(data.slice(at,at+512)));
      if(options.empty)packets.unshift(new Uint8Array());
      if(options.short)packets[0]=packets[0].slice(0,packets[0].length-1);
      if(!options.missingDigest){const digest=new Uint8Array(createHash('md5').update(data).digest());if(options.corrupt)digest[0]^=1;packets.push(digest);}
      return options.reject?1:0;
    },
    transport:{
      async read(){const value=packets.shift();if(!value)return new Promise(()=>{});if(received===data.length)digests++;return value;},
      async write(ack){assert.equal(ack.length,4);received+=Math.min(512,data.length-received);assert.equal(new DataView(ack.buffer).getUint32(0,true),received);}
    }};
  return {loader,get commands(){return commands;},get digests(){return digests;}};
}
const data=randomBytes(32768+91),f=fixture(data);let done=0;
assert.deepEqual(await readS31Flash(f.loader,0x8000,data.length,n=>done=n),new Uint8Array(data));assert.equal(done,data.length);assert.equal(f.digests,1);
assert.deepEqual(await readS31Flash(f.loader,0x10000,data.length),new Uint8Array(data));assert.equal(f.commands,2);assert.equal(f.digests,2);
for(const [options,pattern]of [[{short:true},/长度异常/],[{corrupt:true},/MD5/],[{reject:true},/拒绝/]]){
 const bad=fixture(data,options);await assert.rejects(readS31Flash(bad.loader,0x8000,data.length),pattern);
 await assert.rejects(readS31Flash(bad.loader,0x8000,data.length),/会话已中断/);assert.equal(bad.commands,1);
}
const missing=fixture(data,{missingDigest:true});await assert.rejects(readS31Flash(missing.loader,0x8000,data.length,()=>{},10),/超时/);
assert.deepEqual(await readS31Flash(fixture(data,{empty:true}).loader,0x8000,data.length),new Uint8Array(data));
let deliverLate,lateAcks=0;const late={chip:{IMAGE_CHIP_ID:32},IS_STUB:true,checkCommand:async()=>0,
 transport:{read:()=>new Promise(resolve=>deliverLate=resolve),write:async()=>{lateAcks++;}}};
await assert.rejects(readS31Flash(late,0x8000,512,()=>{},10),/0\/512.*等待数据包.*超时/);
deliverLate(new Uint8Array(512));await new Promise(resolve=>setTimeout(resolve,0));assert.equal(lateAcks,0,'a timed-out read must never ACK late bytes');
// 真实 SDK 解帧，覆盖浏览器碎包、转义字节跨块、响应与数据粘包及逐包确认。
// Exercise real SDK decoding: split WebSerial chunks/escapes, coalesced response/data and stop-and-wait ACKs.
async function serialStreamRegression(Serial){
 const source=Uint8Array.from({length:1553},(_,i)=>i&255),md5=new Uint8Array(createHash('md5').update(source).digest());
 let input,offset=0,acks=0;const device={getInfo:()=>({usbVendorId:0x303a,usbProductId:0x1001})};
 const transport=new Serial(device);transport.trace=()=>{};
 function send(...frames){
  const wire=Uint8Array.from(frames.flatMap(frame=>Array.from(transport.slipWriter(frame))));
  const cuts=[1,2,61,64,3,17];let at=0,part=0;
  while(at<wire.length){const end=Math.min(wire.length,at+cuts[part++%cuts.length]);input.enqueue(wire.slice(at,end));at=end;}
 }
 const next=()=>send(source.slice(offset,Math.min(offset+512,source.length)));
 device.readable=new ReadableStream({start(c){input=c;}});
 device.writable=new WritableStream({write(wire){
  const raw=[];for(let i=1;i<wire.length-1;i++){let b=wire[i];if(b===0xdb)b=wire[++i]===0xdc?0xc0:0xdb;raw.push(b);}
  const bytes=new Uint8Array(raw),view=new DataView(bytes.buffer);
  if(bytes.length===24){assert.equal(bytes[1],0xd2);assert.equal(view.getUint32(16,true),512);assert.equal(view.getUint32(20,true),1);
   // 命令确认和首包来自同一批底层读取。
   // Command response and first data frame share an underlying delivery.
   send(new Uint8Array([1,0xd2,2,0,0,0,0,0,0,0]),source.slice(0,512));
  }else{assert.equal(bytes.length,4);offset+=Math.min(512,source.length-offset);assert.equal(view.getUint32(0,true),offset);acks++;
   if(offset<source.length)next();else send(md5);
  }
 }});
 const loop=transport.readLoop();const loader=new ESPLoader({transport,baudrate:115200,terminal:{clean(){},write(){},writeLine(){}}});
 loader.chip={IMAGE_CHIP_ID:32};loader.IS_STUB=true;
 assert.deepEqual(await readS31Flash(loader,0x8000,source.length),source);assert.equal(acks,4);assert.equal(transport.inWaiting(),0);
 input.close();await loop;
}
await serialStreamRegression(Transport);await serialStreamRegression(S31Transport);
let reads=0;const checksumLoader={chip:{IMAGE_CHIP_ID:32},IS_STUB:true,flashMd5sum:async(address,size)=>{assert.equal(address,0x10000);assert.equal(size,data.length);return createHash('md5').update(data).digest('hex');},transport:{read:async()=>{reads++;throw Error('No full readback allowed');}}};
assert.equal(await verifyS31Flash(checksumLoader,0x10000,new Uint8Array(data)),createHash('md5').update(data).digest('hex'));assert.equal(reads,0);
assert.equal(flashDataMD5(new Uint8Array(data)),createHash('md5').update(data).digest('hex'));
await assert.rejects(verifyS31Flash({...checksumLoader,flashMd5sum:async()=> '0'.repeat(32)},0x10000,new Uint8Array(data)),/MD5 校验失败/);
await assert.rejects(verifyS31Flash({...checksumLoader,flashMd5sum:async()=>''},0x10000,new Uint8Array(data)),/MD5 校验失败/);
const big=fixture(Buffer.alloc(5*1024*1024,0xa5));assert.equal((await readS31Flash(big.loader,0x10000,5*1024*1024)).length,5*1024*1024);assert.equal(big.digests,1);
await assert.rejects(readS31Flash(f.loader,0xffffff,2),/范围/);
const installer=fs.readFileSync(new URL('../flash/metalio-installer.js',import.meta.url),'utf8');
assert.match(installer,/serialOptions:\{bufferSize:65536\}/);assert(!installer.includes('loader.readFlash('),'all installation reads must use the verified reader');
console.log('PASS: actual SDK fragmented/coalesced SLIP and split escapes; single 512-byte frame per ACK, final MD5, sequential reads, large backup and fail-closed errors');
