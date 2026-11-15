// node fl.js <run dir>: failing tests with "px/maxdiff"
const fs=require('fs');const j=JSON.parse(fs.readFileSync(process.argv[2]+'/results.json','utf8'));
for(const k of Object.keys(j).sort()){const x=j[k];if(x&&x.pass>0&&x.fail===0)continue;const f=(x.fails||[]).join(' ');const m=/(\d+) pixels differ, by up to (\d+)/.exec(f);console.log(k+' '+(m?m[1]+'/'+m[2]:'other'));}
