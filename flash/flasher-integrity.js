/* SPDX-License-Identifier: Apache-2.0
 * 中文：按设备校验独立刷机快照，防止缓存、切换版本或跨机型混刷。 / English: Verify independent device snapshots against stale caches, version switches and cross-device images.
 */
(() => {
  const profiles={
    pico:{family:'ESP32-S3',chip:9,project:'Read_Pico',prefix:'Pico-update',offsets:[0,0x8000,0x10000,0xd10000],max:0x400000},
    metalio:{family:'ESP32-S31',chip:32,project:'Kiikoread_Metalio_S31',prefix:'Metalio-S31-update',offsets:[0x2000,0x8000,0x10000,0xd000],max:0x500000}
  };
  const filenames=['bootloader.bin','partitions.bin','firmware.bin','ota_data_initial.bin'];
  const digest=async bytes=>[...new Uint8Array(await crypto.subtle.digest('SHA-256',bytes))].map(v=>v.toString(16).padStart(2,'0')).join('');
  function validate(manifest,device='pico'){
    const model=profiles[device];
    // 版本号接受 rc 小修订（如 0.3.3-rc100.01），由 Anki 线带入：rc100 原正则只认 -rcN，
    // 会拒绝我们带小修订的版本。/ Version accepts rc sub-revisions (e.g. 0.3.3-rc100.01),
    // carried over from the Anki line: rc100's original regex only knew -rcN and would
    // reject our sub-revisioned builds.
    if(!model||!/^\d+\.\d+\.\d+(?:-rc\d+(?:\.\d+)?)?$/.test(manifest.version)||manifest.new_install_prompt_erase!==true||manifest.builds?.length!==1||manifest.builds[0].chipFamily!==model.family||manifest.builds[0].parts?.length!==4)throw Error('刷机清单与所选设备不符，已停止安装。');
    const seen=new Set();
    for(const part of manifest.builds[0].parts){const index=model.offsets.indexOf(part.offset);if(index<0||seen.has(index))throw Error('刷机地址不属于所选设备。');seen.add(index);}
    if(device==='metalio'&&(manifest.board!=='metalio47'||manifest.project!==model.project||manifest.pico_ota?.layout!=='metalio-s31-dual-5m-v1'))throw Error('Metalio 分区或项目身份不匹配。');
    return model;
  }
  function appVersion(bytes,model){
    const view=new DataView(bytes.buffer,bytes.byteOffset,bytes.byteLength);
    if(bytes.length<288||bytes[0]!==0xe9||view.getUint16(12,true)!==model.chip||view.getUint32(32,true)!==0xabcd5432)throw Error('下载文件的芯片身份不匹配。');
    const field=offset=>{const raw=bytes.slice(offset,offset+32);if(!raw.includes(0))throw Error('固件标识损坏。');return new TextDecoder().decode(raw).split('\0')[0];};
    if(field(80)!==model.project)throw Error('固件项目与所选设备不同，已停止安装。');return field(48);
  }
  async function prepare(manifest,manifestURL,archiveFiles,device='pico'){
    const model=validate(manifest,device), snapshot=JSON.parse(JSON.stringify(manifest));
    const urls=[],verified=[];let disposed=false;
    const dispose=()=>{if(disposed)return;disposed=true;urls.forEach(url=>URL.revokeObjectURL(url));};
    try{
      const requests=snapshot.builds[0].parts.map(part=>{
        const index=model.offsets.indexOf(part.offset),name=filenames[index],info=part.sha256?part:archiveFiles?.[name];
        const max=[0x6000,0x1000,model.max,0x2000][index];
        if(!info||!/^[a-f0-9]{64}$/.test(info.sha256)||!Number.isSafeInteger(info.size)||info.size<=0||info.size>max)throw Error('刷机包缺少有效完整性信息。');
        const target=name==='firmware.bin'&&!archiveFiles?`${device==='metalio'?'../':''}${model.prefix}-${manifest.version}.bin`:part.path;
        const url=new URL(target,manifestURL);
        if(url.origin!==new URL(manifestURL).origin||!['https:','http:'].includes(url.protocol))throw Error('刷机文件来源与所选清单不一致。');
        url.searchParams.set('sha256',info.sha256);return {part,name,info,url:url.href};
      });
      const results=await Promise.allSettled(requests.map(async({part,name,info,url})=>{
        const response=await fetch(url,{cache:'no-store'});if(!response.ok)throw Error(`文件下载失败（${response.status}），尚未写入设备。`);
        const data=new Uint8Array(await response.arrayBuffer());
        if(data.length!==info.size||await digest(data)!==info.sha256)throw Error(`${name} 校验失败，已停止安装。`);
        if(name==='firmware.bin'&&appVersion(data,model)!==manifest.version)throw Error('固件内部版本与所选版本不一致。');
        if(name==='bootloader.bin'&&(data[0]!==0xe9||new DataView(data.buffer).getUint16(12,true)!==model.chip))throw Error('引导程序芯片不匹配。');
        verified.push({name,offset:part.offset,data,sha256:info.sha256});
        part.path=URL.createObjectURL(new Blob([data],{type:'application/octet-stream'}));urls.push(part.path);
      }));
      const failure=results.find(r=>r.status==='rejected');if(failure)throw failure.reason;
      const url=URL.createObjectURL(new Blob([JSON.stringify(snapshot)],{type:'application/json'}));urls.push(url);
      return {url,dispose,parts:verified,manifest:snapshot};
    }catch(error){dispose();throw error;}
  }
  window.KiikoreadFlasher=Object.freeze({prepare,validate,digest});
})();
