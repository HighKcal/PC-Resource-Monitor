/* Synthetic, deterministic fixtures. No process, filesystem or network access. */
window.MONITOR_FIXTURE = (() => {
  const GiB = 1073741824;
  const base = (observed, median, upper) => ({ observed, median, upper });
  const processes = [
    { id:'p1', name:'chrome.exe',  pid:12840, group:'browser', cpu:12.4, ram:1.36*GiB, read:240000, write:64000, state:'candidate', path:'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe', baseline:{cpu:base(18.6,2.1,7.8),ram:base(1.32*GiB,.65*GiB,1.1*GiB),read:base(220000,180000,560000),write:base(60000,50000,140000)}, sampleCount:324, days:12, minutes:324, coverage:96, creation:'2026-11-03 13:48:11 KST' },
    { id:'p2', name:'chrome.exe', pid:13212, group:'browser',cpu:4.8,ram:.92*GiB,read:100000,write:0,state:'attention',path:'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe',baseline:{cpu:base(9.4,1.8,6.2),ram:base(.88*GiB,.6*GiB,1.05*GiB),read:base(150000,120000,400000),write:base(0,20000,90000)},sampleCount:290,days:11,minutes:290,coverage:94,creation:'2026-11-03 13:48:12 KST'},
    { id:'p3',name:'chrome.exe',pid:13256,group:'browser',cpu:2.1,ram:.68*GiB,read:0,write:8000,state:'normal',path:'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe'},
    { id:'p4',name:'chrome.exe',pid:13300,group:'browser',cpu:null,ram:.24*GiB,read:null,write:null,state:'reset',reason:'최근 측정 접근 실패',path:'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe',baseline:{cpu:base(8.8,2.1,7.8),ram:base(.23*GiB,.2*GiB,.6*GiB),read:base(120000,90000,280000),write:base(4000,3000,16000)}},
    {id:'p5',name:'chrome.exe',pid:13344,group:'browser',cpu:1.6,ram:.57*GiB,read:32000,write:4000,state:'normal',path:'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe'},
    {id:'p6',name:'chrome.exe',pid:13408,group:'browser',cpu:0,ram:.18*GiB,read:0,write:0,state:'normal',path:'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe'},
    {id:'p7',name:'OneDrive.exe',pid:8940,group:null,cpu:1.2,ram:.28*GiB,read:4200000,write:1700000,state:'learning',path:'C:\\Users\\Example\\AppData\\Local\\Microsoft\\OneDrive\\OneDrive.exe'},
    {id:'p8',name:'SystemTelemetryService.exe',pid:1160,group:null,cpu:null,ram:null,read:null,write:null,state:'unavailable',identityValid:false,noEvidence:true,sampleCount:0,minutes:0,days:0,coverage:0,reason:'프로세스 접근 실패 · 신원 확인 불가',path:null},
    {id:'p9',name:'Research_Dataset_Backup_and_Archive_Validation_Worker_2026_Long_Name.exe',pid:18620,group:null,cpu:3.4,ram:38.5*GiB,read:1234567890,write:98765432,state:'normal',path:'D:\\Research and Development\\Synthetic large-number fixture\\Research_Dataset_Backup_and_Archive_Validation_Worker_2026_Long_Name.exe',large:true},
    {id:'p10',name:'notepad.exe',pid:17860,group:null,cpu:0,ram:.042*GiB,read:0,write:0,state:'learning',path:'C:\\Windows\\System32\\notepad.exe'}
  ];
  for(let i=0;i<34;i++) processes.push({id:`dev${i}`,name:i===0?'Code.exe':i<5?'Code.exe':'node.exe',pid:15400+i*12,group:'development',cpu:i<10?[5.2,1.3,.7,.2,0,.4,.8,.1,.2,.1][i]:i%7===0?null:i%3===0?0:.02,ram:(i<10?[.78,.32,.24,.18,.12,.14,.19,.13,.11,.09][i]:.006)*GiB,read:i%11===0?null:i*1200,write:i%11===0?null:i*240,state:i===0?'normal':i===7?'learning':i>10&&i%7===0?'reset':'normal',path:i<5?'C:\\Users\\Example\\AppData\\Local\\Programs\\Microsoft VS Code\\Code.exe':'C:\\Program Files\\nodejs\\node.exe'});
  for(const p of processes){
    if(!p.baseline)p.baseline={cpu:base((p.cpu??0)*.9,.8,8),ram:base((p.ram??0)*.97,.4*GiB,p.large?45*GiB:1.2*GiB),read:base((p.read??0)*.9,100000,p.large?1500000000:8000000),write:base((p.write??0)*.9,50000,p.large?120000000:4000000)};
    if(p.state==='learning'){p.sampleCount=2;p.days=1;}
    p.sampleCount??=188; p.days??=8; p.minutes=p.sampleCount; p.coverage??=92;
    if(p.identityValid===false)p.creation=null;else p.creation??='2026-11-03 13:52:03 KST';
    // Explicit synthetic dispersion; actual Baseline threshold formula and byte floors.
    const mad={cpu:p.id==='p1'?1.3:p.id==='p2'?1:.6,ram:.08*GiB,read:180000,write:120000};
    if(p.large){Object.assign(p.baseline.ram,{median:36*GiB});Object.assign(p.baseline.read,{median:950000000});Object.assign(p.baseline.write,{median:80000000});Object.assign(mad,{ram:GiB,read:60000000,write:6000000});}
    for(const [metric,floor] of Object.entries({cpu:5,ram:134217728,read:1048576,write:1048576})){
      const b=p.baseline[metric];b.mad=mad[metric];b.upper=b.median+Math.max(3*1.4826*b.mad,.5*b.median,floor);
    }
    p.identityValid??=true;
    if(p.ram!==null)p.ram=Math.round(p.ram);
    // Names and paths are the available collector metadata; no renderer-role inference.
    p.label=p.name;
  }
  return {processes,groups:[{id:'browser',name:'chrome.exe',mark:'C',tone:'chrome',origin:'자동 계통 추정'},{id:'development',name:'Code.exe',mark:'⌘',tone:'code',origin:'자동 계통 추정'}]};
})();
