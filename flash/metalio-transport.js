/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：S31 专用持续串口读取与线性 SLIP 解码；SDK、芯片协议与其他机型保持独立。
 * English: Persistent S31 serial input and linear SLIP decoding, isolated from upstream SDK/chip protocols and other boards.
 */
import {Transport} from './vendor/metalio/esptool-js-0.7.0.js';

export class S31Transport extends Transport{
  async readLoop(){
    if(this.s31LoopRunning)throw Error('串口接收任务重复启动。');
    this.s31LoopRunning=true;this.s31InputError=null;
    let reader;
    try{
      reader=this.reader=this.device.readable.getReader();
      while(true){
        const {value,done}=await reader.read();
        if(done)throw Error('串口接收已结束。');
        if(value?.length){
          if(this.buffer.length+value.length>0x100000)throw Error('串口接收超过安全缓冲范围。');
          this.buffer=this.appendArray(this.buffer,value);
          this.s31Wake?.();
        }
      }
    }catch(error){
      // 发生缓冲溢出或串口错误后不能继续拼接旧半包，必须终止当前会话。
      // Never join a stale partial frame after overrun/serial errors; terminate this session.
      this.s31InputError=error;this.s31Wake?.();
    }finally{
      reader?.releaseLock();if(this.reader===reader)this.reader=undefined;this.s31LoopRunning=false;
    }
  }
  async read(timeout){
    if(this.s31Reading)throw Error('串口读操作重叠。');
    this.s31Reading=true;
    let started=false,escaping=false,length=0,scratch=new Uint8Array(1024);
    const append=byte=>{
      if(length===scratch.length){
        if(length>=65536)throw Error('SLIP 数据包超出安全长度。');
        const larger=new Uint8Array(scratch.length*2);larger.set(scratch);scratch=larger;
      }
      scratch[length++]=byte;
    };
    try{
      while(true){
        if(this.s31InputError)throw Error(`串口输入错误：${this.s31InputError.name}: ${this.s31InputError.message}`);
        if(!this.buffer.length){
          let timer;
          try{
            await new Promise((resolve,reject)=>{
              this.s31Wake=resolve;
              timer=setTimeout(()=>reject(Error(`串口接收超时（本包已解码 ${length} 字节，转义 ${escaping?'未完成':'正常'}）。`)),timeout);
            });
          }finally{clearTimeout(timer);this.s31Wake=null;}
          continue;
        }
        const bytes=this.buffer;this.buffer=new Uint8Array(0);
        for(let i=0;i<bytes.length;i++){
          const byte=bytes[i];
          if(!started){
            if(byte!==0xc0)throw Error(`SLIP 包头异常：0x${byte.toString(16)}。`);
            started=true;
          }else if(escaping){
            escaping=false;
            if(byte===0xdc)append(0xc0);else if(byte===0xdd)append(0xdb);else throw Error('SLIP 转义异常。');
          }else if(byte===0xdb)escaping=true;
          else if(byte===0xc0){
            if(!length)continue;
            // 包尾后的粘包字节保留给下一次读取，不重新解释当前包。
            // Retain coalesced bytes after the delimiter for the next read.
            if(i+1<bytes.length)this.buffer=this.appendArray(bytes.subarray(i+1),this.buffer);
            return scratch.slice(0,length);
          }else append(byte);
        }
      }
    }finally{this.s31Reading=false;}
  }
  async write(data){
    if(!this.device.writable)throw Error('串口发送已断开。');
    const writer=this.device.writable.getWriter();
    try{await writer.write(this.slipWriter(data));}finally{writer.releaseLock();}
  }
}
