'use strict';
(() => {
  const fixture=window.MONITOR_FIXTURE;
  const processes=fixture.processes;
  const $=s=>document.querySelector(s);
  const esc=s=>String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  const metrics={cpu:{name:'CPU',unit:'%',div:1},ram:{name:'메모리',unit:'GB',div:1e9},read:{name:'읽기',unit:'MB/s',div:1e6},write:{name:'쓰기',unit:'MB/s',div:1e6}};
  const state={view:'a',scenario:'mixed',paused:false,selected:'p1',expanded:new Set(['browser']),sort:'cpu',desc:true,search:'',flat:false,metric:'cpu',suppressed:{},owners:Object.fromEntries(processes.map(p=>[p.id,p.group])),manual:new Set()};
  const groups=[...fixture.groups,{id:'shared',name:'분리한 공유 프로세스',mark:'↗',tone:'shared',origin:'사용자 지정 · 세션'}];
  const byId=id=>processes.find(p=>p.id===id);
  const members=id=>processes.filter(p=>state.owners[p.id]===id);
  const isFirst=()=>state.scenario==='first';
  const isCollectionError=()=>state.scenario==='collection-error';
  function value(p,m){if(isCollectionError())return null;if(isFirst()&&m!=='ram')return null;return p[m];}
  function format(n,m,unit=false){
    if(n===null||!Number.isFinite(n))return '—';
    const v=n/metrics[m].div;
    const threshold=m==='cpu'?.1:.01;
    const result=v>0&&v<threshold?(m==='cpu'?'<0.1':'<0.01'):v.toLocaleString('ko-KR',{minimumFractionDigits:m==='cpu'?1:2,maximumFractionDigits:m==='cpu'?1:2});
    return result+(unit?' '+metrics[m].unit:'');
  }
  function sum(ps,m){const values=ps.map(p=>value(p,m)).filter(v=>v!==null);return {n:values.length?values.reduce((a,b)=>a+b,0):null,valid:values.length,total:ps.length};}
  function status(p,m=state.metric){
    if(isCollectionError())return {kind:'unavailable',text:'수집 오류 · 관측 불가'};
    if(state.scenario==='storage-error')return {kind:'unavailable',text:'저장 오류 · 수집 중단'};
    if(isFirst()&&m!=='ram')return {kind:'unavailable',text:'첫 표본 · 변화량 대기'};
    if(p.state==='unavailable')return {kind:'unavailable',text:'접근 실패 · 비교 불가'};
    if(p.state==='reset'&&value(p,m)===null)return {kind:'unavailable',text:'최근 결측 · 비교 중단'};
    if(p.state==='learning'||isFirst())return {kind:'learning',text:'비교 자료 수집 중'};
    const b=p.baseline[m];
    if(b.observed>b.upper){const candidate=p.state==='candidate';return {kind:candidate?'candidate':'attention',text:candidate?'평소보다 높음':'상한 초과 · 1/3창'};}
    return {kind:'normal',text:'비교 상한 이내'};
  }
  function badge(s){return `<span class="status ${s.kind}"><span></span>${esc(s.text)}</span>`;}
  function groupInfo(g){return state.manual.has(g.id)?'사용자 지정 · 세션':g.origin;}
  function icon(p){return `<span class="process-icon ${p.name==='chrome.exe'?'chrome':p.name==='Code.exe'?'code':p.large?'archive':''}" aria-hidden="true">${p.name==='chrome.exe'?'C':p.name==='Code.exe'?'⌘':p.name==='node.exe'?'N':p.name==='OneDrive.exe'?'☁':p.large?'↗':p.name==='notepad.exe'?'≡':'·'}</span>`;}
  function renderObservation(){
    const paused=state.paused;
    const error=isCollectionError();
    $('#pause').innerHTML=paused?'▶ 표시 재개':'Ⅱ 표시 일시정지';
    $('#pause').classList.toggle('resume',paused);
    $('#observation').innerHTML=`<div class="observation-state"><span class="state-light ${paused?'paused':error?'error':''}"></span><div><strong>${paused?'표시 일시정지':error?'수집 오류 예시':state.scenario==='storage-error'?'저장 오류 · 오류 전 마지막 표시':'최근 표본 · 고정 예시'}</strong><span>${paused?'현재 화면과 세션 구성을 보존합니다':error?'오류 시각 14:32:09 KST · 마지막 성공 14:32:06':state.scenario==='storage-error'?'수집 중단 · 오류 시각 14:32:09 KST':'실시간 수집 없이 동일한 표본을 살펴봅니다'}</span></div></div><div class="observation-meta"><div><span>표본 시각 · KST</span><strong>14:32:06</strong><small>UTC 05:32:06</small></div><div><span>실제 관측 간격</span><strong>${isFirst()?'첫 표본':error?'관측 불가':'3.04 <small>초</small>'}</strong><small>QPC 기준 · 예시</small></div><div><span>개별 비교 기준</span><strong>마지막 완료창</strong><small>최근 표본과 별도</small></div></div>`;
    const banner=$('#scenario-banner');
    const messages={first:'첫 표본 예시 · CPU와 읽기/쓰기는 변화량을 기다립니다. 메모리 Working Set은 독립적으로 표시하며, 현재 비교 상태는 아직 준비되지 않았습니다.','collection-error':'수집 오류 예시 · 최근 수치는 측정 불가입니다. 오류 전 완료창 수치를 보존해 보여 주는 것은 이번 UI 제안입니다. 보존 수치로 현재 비교 상태를 복원하지 않습니다.','storage-error':'저장 오류 예시 · 저장 실패로 수집이 중단되었습니다. 아래는 오류 전 마지막 표시이며 현재 관측이 아닙니다. 오류 전 값 보존은 이번 UI 제안입니다.'};
    banner.hidden=!messages[state.scenario];
    banner.textContent=messages[state.scenario]||'';
    banner.className='scenario-banner '+(state.scenario.includes('error')?'error-banner':'');
  }
  function matching(p){return !state.search||`${p.name} ${p.pid} ${p.path||''}`.toLowerCase().includes(state.search);}
  function sorted(items){return items.sort((a,b)=>{
    let av,bv;
    if(state.sort==='name'){av=a.name;bv=b.name;return av.localeCompare(bv,'ko')*(state.desc?-1:1);}
    av=a.group===true?sum(members(a.id),state.sort).n:value(a,state.sort);
    bv=b.group===true?sum(members(b.id),state.sort).n:value(b,state.sort);
    if(av===null&&bv===null)return 0;if(av===null)return 1;if(bv===null)return -1;
    return (av-bv)*(state.desc?-1:1);
  });}
  function numericCells(ps,group){return Object.keys(metrics).map(m=>{const s=sum(ps,m);return `<td class="numeric ${m==='cpu'?'cpu-cell':''}"><span>${esc(format(s.n,m))}</span>${group&&s.valid<s.total?`<small class="partial">${s.valid}/${s.total} ${s.valid?'부분 합계':'측정 불가'}</small>`:s.n===null?'<small>측정 불가</small>':''}</td>`;}).join('');}
  function processRow(p,child=false){const active=state.selected===p.id;const s=status(p,'cpu');const short=s.kind==='candidate'?'평소보다 높음':s.kind==='attention'?'상한 초과 1/3':s.kind==='normal'?'상한 이내':s.kind==='learning'?'자료 수집 중':isFirst()?'첫 표본':'비교 중단';return `<tr class="process-row ${child?'child-row':''} ${active?'selected':''}" data-id="${p.id}" tabindex="${active?'0':'-1'}" aria-selected="${active}"><td><button class="row-select" data-select="${p.id}" aria-label="${esc(p.name)} PID ${p.pid} 상세"><span class="child-branch" aria-hidden="true">${child?'↳':''}</span>${icon(p)}<span class="row-name"><strong title="${esc(p.name)}">${esc(p.name)}</strong><small class="process-meta" title="CPU 비교 · 마지막 완료창: ${esc(s.text)}"><span>PID ${p.pid}</span><span class="inline-state ${s.kind}">${short}</span></small></span><span class="row-status-dot ${status(p,'cpu').kind}" title="CPU: ${esc(status(p,'cpu').text)}"></span></button></td>${numericCells([p],false)}</tr>`;}
  function renderTable(){
    $('#process-count').textContent=`${processes.length}개 프로세스`;
    const entities=[];
    if(state.flat){entities.push(...processes.filter(matching));}
    else{
      for(const g of groups){const ps=members(g.id);if(ps.length&&(ps.some(matching)||g.name.toLowerCase().includes(state.search)))entities.push({...g,group:true});}
      entities.push(...processes.filter(p=>!state.owners[p.id]&&matching(p)));
    }
    let html='';
    for(const e of sorted(entities)){
      if(e.group!==true){html+=processRow(e);continue;}
      const ps=members(e.id),expanded=state.expanded.has(e.id),active=state.selected===`g:${e.id}`;
      html+=`<tr class="group-row ${active?'selected':''}" data-id="g:${e.id}" tabindex="${active?'0':'-1'}" aria-selected="${active}"><td><div class="group-cell"><button class="expand" data-expand="${e.id}" aria-expanded="${expanded}" aria-label="${esc(e.name)} 구성원 ${expanded?'접기':'펼치기'}">${expanded?'⌄':'›'}</button><button class="row-select group-select" data-select="g:${e.id}"><span class="process-icon ${e.tone}" aria-hidden="true">${e.mark}</span><span class="row-name"><strong>${esc(e.name)} <span class="member-count">${ps.length}</span></strong><small>${esc(groupInfo(e))}</small></span></button></div></td>${numericCells(ps,true)}</tr>`;
      if(expanded){const matchingGroup=e.name.toLowerCase().includes(state.search);const shown=sorted([...ps]).filter(p=>matchingGroup||matching(p));if(state.search&&shown.length<ps.length)html+=`<tr class="search-scope"><td colspan="5">검색 결과 ${shown.length}/${ps.length}개 표시 · 위 그룹 합계는 전체 ${ps.length}개 기준</td></tr>`;html+=shown.map(p=>processRow(p,true)).join('');}
    }
    $('#rows').innerHTML=html;
    $('#empty').hidden=entities.length!==0;
    $('#resource-table').hidden=entities.length===0;
    if(!$('#rows tr[tabindex="0"]'))$('#rows tr[tabindex]')?.setAttribute('tabindex','0');
    document.querySelectorAll('[data-sort]').forEach(b=>{const active=b.dataset.sort===state.sort;b.querySelector('span').textContent=active?(state.desc?'↓':'↑'):'';b.closest('th').setAttribute('aria-sort',active?(state.desc?'descending':'ascending'):'none');});
  }
  function metricSelector(){return `<div class="metric-tabs" role="group" aria-label="완료창 비교 지표">${Object.entries(metrics).map(([k,m])=>`<button data-metric="${k}" aria-pressed="${k===state.metric}">${m.name}</button>`).join('')}</div>`;}
  function comparisonBars(b,m){const max=Math.max(b.observed,b.upper,b.median)*1.15||1;return `<div class="comparison-chart" aria-label="완료창 관측값, 과거 중앙값, 비교 상한"><div class="bar-row observed"><span>완료창 관측</span><div class="bar-track"><i style="width:${b.observed/max*100}%"></i></div><strong>${format(b.observed,m,true)}</strong></div><div class="bar-row"><span>과거 중앙값</span><div class="bar-track"><i style="width:${b.median/max*100}%"></i></div><strong>${format(b.median,m,true)}</strong></div><div class="bar-row upper"><span>비교 상한</span><div class="bar-track"><i style="width:${b.upper/max*100}%"></i></div><strong>${format(b.upper,m,true)}</strong></div></div>`;}
  function renderGroupDetail(g){const ps=members(g.id);return `<div class="detail-top"><span class="eyebrow">CURRENT GROUP</span><button class="icon-button" data-detail-close aria-label="그룹 상세 닫기">×</button></div><div class="detail-identity"><span class="process-icon large ${g.tone}">${g.mark}</span><div><h2>${esc(g.name)}</h2><p>${esc(groupInfo(g))} · ${ps.length}개 구성원</p></div></div><div class="group-explainer"><h3>함께 표시한 현재 사용량</h3><p>이 그룹에는 별도 Baseline이나 합산 비교 상태가 없습니다. 개별 구성원을 선택하면 그 프로세스의 근거를 볼 수 있습니다.</p></div><div class="current-grid">${Object.keys(metrics).map(m=>{const s=sum(ps,m);return `<div><span>${metrics[m].name}</span><strong>${format(s.n,m)} <small>${metrics[m].unit}</small></strong><small>${s.valid}/${s.total}개 유효${s.valid<s.total?' · 부분 합계':''}</small></div>`;}).join('')}</div><div class="detail-section"><h3>현재 구성원 <span>${ps.length}</span></h3><div class="member-list">${ps.map(p=>`<button data-select="${p.id}">${icon(p)}<span>${esc(p.name)}<small>PID ${p.pid}</small></span><span>→</span></button>`).join('')}</div></div><div class="detail-section"><p class="fine-print">단일 소속 기준으로 계산합니다. 공유 페이지가 중복될 수 있으며, 각 프로세스의 실제 관측 간격은 다를 수 있습니다. 검색해도 합계 범위는 바뀌지 않습니다.</p><button class="button full" data-group-open="${g.id}">현재 구성원 지정·이동</button><p class="fine-print">세션 한정 · 표시 재개 시 자동 구성으로 초기화</p></div>`;}
  function renderProcessDetail(p){
    const m=state.metric,b=p.baseline[m],s=status(p,m),unavailable=s.kind==='unavailable',learning=s.kind==='learning',noEvidence=isFirst()||p.noEvidence;
    const mad=b.mad;
    const exact=p.path?state.suppressed[p.path]:null;
    const owner=groups.find(g=>g.id===state.owners[p.id]);
    const gapText=isCollectionError()?'이번 회차 수집 오류':state.scenario==='storage-error'?'저장 오류 · 오류 전 마지막 표시':isFirst()?'첫 표본 · 변화량 대기':p.state==='reset'?'최근 측정 접근 실패':p.reason||'측정 불가';
    return `<div class="detail-top"><span class="eyebrow">PROCESS EVIDENCE</span><button class="icon-button" data-detail-close aria-label="프로세스 상세 닫기">×</button></div><div class="detail-identity">${icon(p)}<div><h2 title="${esc(p.name)}">${esc(p.name)}</h2><p>PID ${p.pid} <span>·</span> ${owner?esc(owner.name):'독립 표시'}</p></div></div><div class="detail-current"><div class="mini-heading"><h3>${state.scenario==='storage-error'?'오류 전 마지막 표본':'최근 표본'}</h3><span>14:32:06 KST · ${isFirst()?'첫 표본':isCollectionError()||!p.identityValid?'관측 간격 확인 불가':'3.04초 간격'}</span></div><div class="current-grid">${Object.keys(metrics).map(k=>`<div><span>${metrics[k].name}</span><strong>${format(value(p,k),k)} <small>${metrics[k].unit}</small></strong>${value(p,k)===null?'<small class="invalid">측정 불가 · 0 아님</small>':''}</div>`).join('')}</div>${isFirst()?'<p class="fine-print">CPU·I/O는 두 유효 표본의 변화량이 필요합니다. 메모리는 현재 Working Set입니다.</p>':''}</div><div class="comparison-section"><div class="mini-heading"><h3>마지막 완료창 비교</h3><span class="proposal">근거 보기 제안</span></div>${metricSelector()}<div class="comparison-state">${badge(s)}<p>${noEvidence?(isFirst()?'새 세션에서 아직 완료된 관측창이 없습니다. 이 화면의 비교 자료는 미조회 상태이며, 기존 저장 이력은 유지됩니다.':'신원이 확인되지 않아 연결 가능한 완료창과 비교 근거가 없습니다.'):unavailable?(state.scenario==='storage-error'?'저장 실패 후 비교 갱신 중단; 아래는 오류 전 표시입니다.':isCollectionError()?'수집 실패 후 비교 갱신 중단; 아래는 오류 전 표시입니다.':'최근 측정 결측으로 비교 상태 중단; 아래는 보존된 완료창 수치입니다.'):learning?'기준을 만들기 위한 유효한 과거 자료가 아직 충분하지 않습니다.':s.kind==='candidate'?'완료창 관측값이 과거 상한을 3개 창 연속 넘었습니다.':s.kind==='attention'?'완료창 관측값이 상한을 넘었습니다. 연속 3개 창을 아직 충족하지 않았습니다.':'완료창 관측값이 이 프로세스의 과거 비교 상한 이내입니다.'}</p></div>${unavailable&&!noEvidence?`<div class="missing-reason">현재 중단 사유: ${esc(gapText)}<br>현재 연속 초과: 미확정 · 보존 수치로 복원하지 않음</div>`:''}${noEvidence?`<div class="learning-evidence"><span class="learning-mark">◌</span><strong>아직 완료창 없음</strong><p>${isFirst()?'첫 표본 · 유효한 변화량을 기다립니다':'신원을 확인하지 못해 비교 근거를 연결할 수 없습니다'}<br>${isFirst()?'새 세션의 완료창 0개<br>이 화면의 비교 자료 미조회 · 기존 저장 이력 유지':'연결 가능한 완료창 없음 · 신원 확인 필요'}</p></div>`:learning?`<div class="learning-evidence"><span class="learning-mark">◌</span><strong>비교할 과거가 더 필요해요</strong><p>현재 자료: 유효 완료창 ${p.sampleCount}개 · UTC ${p.days}일 · 서로 다른 분 ${p.minutes}개</p><p>비교 준비 조건: 유효 완료창 60개 이상 · UTC 날짜 3일 이상 · 서로 다른 분 60개 이상</p><p class="fine-print">날짜는 UTC 기준이며, 완료 예정 시간을 뜻하지 않습니다. 측정값은 계속 표시하며 결측을 0으로 채우지 않습니다.</p></div>`:`<div class="window-time"><span>${unavailable?'보존된':'완료된'} 관측창</span><strong>14:30:51 — 14:32:03 KST</strong><small>UTC 05:30:51 — 05:32:03 · 실제 길이 72.0초</small></div>${comparisonBars(b,m)}<div class="baseline-facts"><div><span>과거 분대표</span><strong>${p.sampleCount}개</strong></div><div><span>과거 자료</span><strong>UTC ${p.days}일 · ${p.minutes}분</strong></div><div><span>완료창 유효 표본</span><strong>24개 · ${p.coverage}%</strong></div><div><span>과거 MAD</span><strong>${format(mad,m,true)}</strong></div></div><p class="fine-print">현재 창 이전의 과거 자료만 사용한 합성 비교입니다. 상한 = 중앙값 + max(3 × 1.4826 × MAD, 중앙값의 50%, 지표별 최소폭). 최소폭: CPU 5%p, 메모리 134,217,728 bytes, I/O 1,048,576 B/s. ${m==='ram'?'과거·현재 메모리는 Working Set bytes를 GB로 표시합니다.':''} ${unavailable?'보존된 숫자는 현재 판정이 아닙니다.':''}</p>`}</div><div class="detail-section feedback-section"><div class="mini-heading"><h3>이 경로의 알림만 잠시 숨기기</h3><span class="session-tag">예시 조작</span></div><p>측정·학습·비교 사실은 그대로 둡니다. 개별 실행 파일의 정확한 경로에 적용합니다.</p>${exact?`<div class="suppression-note">${exact}시간 억제 예시 적용 중 <button data-feedback="cancel">취소</button></div>`:''}<div class="feedback-actions"><button class="button" data-feedback="1" ${!p.path?'disabled':''}>1시간</button><button class="button" data-feedback="24" ${!p.path?'disabled':''}>24시간</button></div>${!p.path?'<p class="fine-print">검증 가능한 경로가 없어 적용할 수 없습니다.</p>':'<p class="fine-print">브라우저 예시 메모리만 변경 · 제품 DB 저장 없음</p>'}</div><div class="detail-section identity-section"><h3>대상 확인</h3><dl><dt>생성 시각 · 예시</dt><dd>${p.creation?esc(p.creation):'확인 불가 · 생성시각 미확인'}</dd><dt>정확한 경로 · 합성 예시</dt><dd class="path">${p.path?esc(p.path):'확인 불가 · 신원 불확실'}</dd>${p.ram!==null?`<dt>현재 Working Set 원본</dt><dd>${Math.round(p.ram).toLocaleString('ko-KR')} bytes<br><small>1 GB = 1,000,000,000 bytes · 공유 페이지 중복 가능</small></dd>`:''}</dl><button class="text-button" data-group-open="${state.owners[p.id]||'browser'}" ${!p.identityValid?'disabled':''}>이 프로세스의 소속 편집 ↗</button>${!p.identityValid?'<p class="fine-print">신원을 확인해야 소속을 바꿀 수 있습니다.</p>':''}</div>`;
  }
  function renderDetail(){
    const pane=$('#detail');
    if(!state.selected){pane.innerHTML='<div class="detail-empty"><span>↖</span><h2>프로세스를 선택하세요</h2><p>목록에서 개별 프로세스를 선택하면<br>최근 표본과 완료창 비교를 볼 수 있습니다.</p></div>';return;}
    if(state.selected.startsWith('g:')){const g=groups.find(g=>g.id===state.selected.slice(2));pane.innerHTML=renderGroupDetail(g);}
    else{const p=byId(state.selected);pane.innerHTML=renderProcessDetail(p);}
  }
  function renderComparisonIntro(){
    const el=$('#comparison-intro');el.hidden=state.view!=='b';if(el.hidden)return;
    const picks=['p1','p4','p7'];
    el.innerHTML=`<div class="section-heading comparison-heading"><div><span class="section-index">01</span><h2>개별 프로세스 비교</h2></div><span>${metrics[state.metric].name} · 마지막 완료창 <span class="proposal inline">새 UI 제안</span></span></div><div class="comparison-cards">${picks.map(id=>{const p=byId(id),s=status(p),b=p.baseline[state.metric],unavailable=s.kind==='unavailable',learning=s.kind==='learning',noEvidence=isFirst()||p.noEvidence;return `<button class="comparison-card ${state.selected===id?'chosen':''}" data-select="${id}"><span class="card-top">${icon(p)}<strong>${esc(p.name)}</strong><span>↗</span></span><small>PID ${p.pid} · ${metrics[state.metric].name} 완료창 비교</small>${badge(s)}<span class="card-value">${noEvidence?'완료창 없음':learning?'학습 중':unavailable?'비교 중단':format(b.observed,state.metric,true)}</span><span class="card-caption">${noEvidence?'새 세션 · 비교 자료 미조회 · 저장 이력 유지':learning?'유효한 과거 자료를 더 모읍니다':unavailable?'보존 수치와 현재 상태를 구분합니다':'과거 중앙값 '+format(b.median,state.metric,true)+' · 상한 '+format(b.upper,state.metric,true)}</span></button>`;}).join('')}</div>`;
  }
  function render(){document.body.dataset.view=state.view;$('#page-title').textContent=state.view==='a'?'지금, 무엇을 쓰고 있나요?':'평소와 다른 사용, 근거부터.';$('#page-subtitle').textContent=state.view==='a'?'프로그램의 최근 사용량과 개별 프로세스의 평소 기준을 함께 살펴보세요.':'완료된 관측창을 과거 기준과 비교하고, 현재 사용량까지 이어서 확인하세요.';document.querySelectorAll('[data-view]').forEach(b=>b.setAttribute('aria-pressed',b.dataset.view===state.view));renderObservation();renderTable();renderDetail();renderComparisonIntro();}
  function notify(message){$('#toast').textContent=message;$('#toast').classList.add('visible');clearTimeout(notify.timer);notify.timer=setTimeout(()=>$('#toast').classList.remove('visible'),4500);}
  function select(id){const fromCard=document.activeElement?.classList.contains('comparison-card');state.selected=id;renderTable();renderDetail();renderComparisonIntro();const target=fromCard?document.querySelector(`.comparison-card[data-select="${id}"]`):document.querySelector(`#rows tr[data-id="${id}"]`);target?.focus({preventScroll:true});}
  function pauseToggle(){if(state.paused){state.paused=false;state.scenario='first';$('#scenario').value='first';state.owners=Object.fromEntries(processes.map(p=>[p.id,p.group]));state.manual.clear();state.selected=null;state.expanded.clear();notify('표시 재개 예시: 세션 소속·선택·펼침을 초기화했습니다. 첫 변화량을 기다립니다.');}else{state.paused=true;notify('표시를 고정했습니다. 합성 예시이므로 실제 수집은 없습니다.');}render();}
  function openGroup(id){
    const currentGroup=groups.some(g=>g.id===id)?id:'browser';
    const selected=state.selected&&!state.selected.startsWith('g:')&&byId(state.selected)?.identityValid?state.selected:'p1';
    $('#group-dialog').innerHTML=`<div class="dialog-top"><span class="eyebrow">SESSION GROUPING</span><button class="icon-button" data-close aria-label="그룹 편집 닫기">×</button></div><h2 id="group-title">현재 구성원 편집 <span class="proposal">편집 배치 제안</span></h2><p class="dialog-lead">명시한 소속은 이 세션에서만 유지됩니다. 한 프로세스는 한 그룹에만 포함됩니다.</p><div class="group-editor-top"><label>편집할 그룹<select id="edit-group">${groups.map(g=>`<option value="${g.id}" ${currentGroup===g.id?'selected':''}>${esc(g.name)}</option>`).join('')}</select></label><span id="edit-count"></span></div><div class="membership-list" id="membership-list"></div><div class="membership-note">선택한 항목을 이 그룹으로 이동합니다. 선택 해제한 기존 구성원은 독립 표시합니다. 다른 그룹에서 옮겨온 항목은 원래 그룹 합계에서 빠집니다.</div><div class="single-move"><h3>개별 대상 이동·공유 분리</h3><div><select id="move-process" aria-label="이동할 프로세스">${processes.map(p=>`<option value="${p.id}" ${selected===p.id?'selected':''} ${!p.identityValid?'disabled':''}>${esc(p.name)} · PID ${p.pid}${!p.identityValid?' · 신원 미확인(이동 불가)':''}</option>`).join('')}</select><select id="move-destination" aria-label="대상 그룹"><option value="">독립 표시</option>${groups.map(g=>`<option value="${g.id}">${esc(g.name)}</option>`).join('')}</select><button class="button" id="move-one">이동</button></div></div><div class="group-dialog-footer"><button class="text-button" id="restore-auto">모든 소속 자동 복원</button><button class="button primary" id="save-membership">선택한 구성원 적용</button></div><div class="dialog-footnote">Pause는 표시와 구성을 보존합니다. Resume(표시 재개)과 새로고침은 사용자 세션 지정을 초기화합니다. 그룹 비교·그룹 피드백은 없습니다.</div>`;
    populateMembership();$('#group-dialog').showModal();
  }
  function populateMembership(){const id=$('#edit-group').value;$('#membership-list').innerHTML=processes.map(p=>`<label><input type="checkbox" name="member" value="${p.id}" ${state.owners[p.id]===id?'checked':''} ${!p.identityValid?'disabled':''}>${icon(p)}<span><strong>${esc(p.name)}</strong><small>PID ${p.pid} · ${!p.identityValid?'신원 미확인 · 소속 변경 불가':'현재 '+esc(groups.find(g=>g.id===state.owners[p.id])?.name||'독립 표시')}</small></span></label>`).join('');$('#edit-count').textContent=`현재 ${members(id).length}개 구성원`;}
  document.addEventListener('click',event=>{
    const b=event.target.closest('button');if(!b)return;
    if(b.dataset.view){state.view=b.dataset.view;render();}
    else if(b.dataset.expand){const id=b.dataset.expand;state.expanded.has(id)?state.expanded.delete(id):state.expanded.add(id);renderTable();$(`[data-expand="${id}"]`)?.focus();}
    else if(b.dataset.select){select(b.dataset.select);}
    else if(b.hasAttribute('data-detail-close')){state.selected=null;renderTable();renderDetail();renderComparisonIntro();$('#rows .row-select')?.focus();}
    else if(b.dataset.metric){state.metric=b.dataset.metric;renderDetail();renderComparisonIntro();$(`[data-metric="${state.metric}"]`)?.focus();}
    else if(b.dataset.sort){if(state.sort===b.dataset.sort)state.desc=!state.desc;else{state.sort=b.dataset.sort;state.desc=b.dataset.sort!=='name';}renderTable();}
    else if(b.hasAttribute('data-feedback')){const p=byId(state.selected);if(!p?.path)return;if(b.dataset.feedback==='cancel')delete state.suppressed[p.path];else state.suppressed[p.path]=Number(b.dataset.feedback);renderDetail();notify(b.dataset.feedback==='cancel'?'이 정확한 경로의 억제 예시를 취소했습니다.':`이 정확한 경로에 ${b.dataset.feedback}시간 알림 억제 예시를 적용했습니다. 같은 경로의 프로세스에 공유됩니다.`);}
    else if(b.hasAttribute('data-group-open'))openGroup(b.dataset.groupOpen);
    else if(b.hasAttribute('data-close'))b.closest('dialog').close();
    else if(b.id==='scope-open')$('#scope-dialog').showModal();
    else if(b.id==='pause')pauseToggle();
    else if(b.id==='group-edit')openGroup(state.selected?.startsWith('g:')?state.selected.slice(2):state.owners[state.selected]||'browser');
    else if(b.id==='clear-search'){$('#search').value='';state.search='';renderTable();$('#search').focus();}
    else if(b.id==='save-membership'){const id=$('#edit-group').value;const checked=new Set([...document.querySelectorAll('[name="member"]:checked')].map(e=>e.value));for(const p of processes){if(!p.identityValid)continue;if(checked.has(p.id)){if(state.owners[p.id])state.manual.add(state.owners[p.id]);state.owners[p.id]=id;}else if(state.owners[p.id]===id)state.owners[p.id]=null;}state.manual.add(id);state.expanded.add(id);$('#group-dialog').close();render();notify('현재 구성원을 적용했습니다. 합계는 단일 소속 기준으로 다시 계산했습니다.');}
    else if(b.id==='move-one'){const p=$('#move-process').value,d=$('#move-destination').value;if(!byId(p)?.identityValid){notify('신원을 확인하지 못한 프로세스는 소속을 변경할 수 없습니다.');return;}if(state.owners[p])state.manual.add(state.owners[p]);state.owners[p]=d||null;if(d)state.manual.add(d);populateMembership();render();notify('선택한 프로세스를 이동했습니다. 현재 세션에서만 유지됩니다.');}
    else if(b.id==='restore-auto'){state.owners=Object.fromEntries(processes.map(p=>[p.id,p.group]));state.manual.clear();populateMembership();render();notify('예시의 초기 자동 계통 추정 구성으로 복원했습니다.');}
  });
  $('#scenario').addEventListener('change',e=>{state.scenario=e.target.value;state.paused=state.scenario==='paused';render();});
  $('#search').addEventListener('input',e=>{state.search=e.target.value.trim().toLowerCase();renderTable();});
  $('#flat').addEventListener('change',e=>{state.flat=e.target.checked;renderTable();});
  $('#group-dialog').addEventListener('change',e=>{if(e.target.id==='edit-group')populateMembership();});
  document.addEventListener('keydown',e=>{
    if((e.ctrlKey||e.metaKey)&&e.key.toLowerCase()==='f'&&!document.querySelector('dialog[open]')){e.preventDefault();$('#search').focus();$('#search').select();}
    if(e.key==='Escape'&&!document.querySelector('dialog[open]')){state.selected=null;renderDetail();renderTable();renderComparisonIntro();$('#rows .row-select')?.focus();}
    if(document.querySelector('dialog[open]'))return;
    const row=e.target.closest('#rows tr[tabindex]');if(!row)return;
    const all=[...document.querySelectorAll('#rows tr[tabindex]')],index=all.indexOf(row);
    if(e.key==='ArrowDown'||e.key==='ArrowUp'){e.preventDefault();const next=all[Math.max(0,Math.min(all.length-1,index+(e.key==='ArrowDown'?1:-1)))];select(next.dataset.id);$(`#rows tr[data-id="${next.dataset.id}"]`)?.focus();}
    else if(e.key==='ArrowRight'||e.key==='ArrowLeft'){if(row.dataset.id.startsWith('g:')){e.preventDefault();const id=row.dataset.id.slice(2);e.key==='ArrowRight'?state.expanded.add(id):state.expanded.delete(id);renderTable();$(`#rows tr[data-id="g:${id}"]`)?.focus();}}
    else if((e.key==='Enter'||e.key===' ')&&e.target===row){e.preventDefault();select(row.dataset.id);$(`#rows tr[data-id="${row.dataset.id}"]`)?.focus();}
  });
  render();
})();
