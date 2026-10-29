/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：回归持续读锁、边界解码、错误传播、写锁释放及原生 stub 完整性。
 * English: Regress persistent read locks, boundary decoding, error propagation, write-lock release and native stub integrity.
 */
import assert from 'node:assert/strict';
import {S31Transport} from '../flash/metalio-transport.js';
let input,locks=0;const raw=new ReadableStream({start(c){input=c;}});
const originalReader=raw.getReader.bind(raw);raw.getReader=()=>{locks++;return originalReader();};
const port={readable:raw,writable:new WritableStream({}),close:async()=>{port.readable=null;port.writable=null;}};
const serial=new S31Transport(port);const loop=serial.readLoop();
for(let i=0;i<256;i++){
 const payload=Uint8Array.from({length:512},(_,n)=>(n+i)&255),wire=serial.slipWriter(payload);
 const result=serial.read(1000);
 for(let at=0;at<wire.length;at+=61)input.enqueue(wire.subarray(at,at+61));
 assert.deepEqual(await result,payload);
}
assert.equal(locks,1,'serial pump must keep one reader through all chunks');
const fault=serial.read(1000);input.error(new DOMException('input overrun','BufferOverrunError'));
await assert.rejects(fault,/BufferOverrunError/);await loop;
assert.equal(raw.locked,false);
const writable=new WritableStream({write(){throw Error('disconnected');}}),tx=new S31Transport({writable});
await assert.rejects(tx.write(new Uint8Array([1,2,3])),/disconnected/);assert.equal(writable.locked,false);
const invalid=new S31Transport({});invalid.buffer=new Uint8Array([0xc0,0xdb,0x55,0xc0]);
await assert.rejects(invalid.read(100),/转义异常/);
const partial=new S31Transport({});partial.buffer=new Uint8Array([0xc0,1,2,0xdb]);
await assert.rejects(partial.read(10),/本包已解码 2 字节，转义 未完成/);
console.log('PASS: one persistent WebSerial reader, 256 escaped packets, partial-frame diagnostics, fatal RX errors and write-lock cleanup');
