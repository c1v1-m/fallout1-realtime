// stack crops (1x coords x0..x1, y0..y1) of several 3x slot pngs, scaled K more
const fs = require('fs'), zlib = require('zlib');
function readPng(f){const b=fs.readFileSync(f);let p=8,w,h,idat=[];while(p<b.length){const len=b.readUInt32BE(p),t=b.toString('ascii',p+4,p+8);const d=b.subarray(p+8,p+8+len);if(t==='IHDR'){w=d.readUInt32BE(0);h=d.readUInt32BE(4);}if(t==='IDAT')idat.push(d);p+=12+len;}const raw=zlib.inflateSync(Buffer.concat(idat));const px=Buffer.alloc(w*h*3);for(let y=0;y<h;y++)raw.copy(px,y*w*3,y*(w*3+1)+1,(y+1)*(w*3+1));return{w,h,px};}
const [out, x0, x1, y0, y1, K, ...files] = process.argv.slice(2);
const X0=x0*3,X1=x1*3,Y0=y0*3,Y1=y1*3,k=Number(K),CW=(X1-X0)*k,CH=(Y1-Y0)*k,GAP=10;
const W=CW,H=(CH+GAP)*files.length;const px=Buffer.alloc(W*H*3,25);
files.forEach((f,i)=>{const im=readPng(f);for(let y=0;y<CH;y++)for(let x=0;x<CW;x++){const sx=X0+Math.floor(x/k),sy=Y0+Math.floor(y/k);const o=(sy*im.w+sx)*3;const d=((i*(CH+GAP)+y)*W+x)*3;px[d]=im.px[o];px[d+1]=im.px[o+1];px[d+2]=im.px[o+2];}});
const raw=Buffer.alloc((W*3+1)*H);for(let y=0;y<H;y++){raw[y*(W*3+1)]=0;px.copy(raw,y*(W*3+1)+1,y*W*3,(y+1)*W*3);}
const crcT=[];for(let n=0;n<256;n++){let c=n;for(let q=0;q<8;q++)c=c&1?0xedb88320^(c>>>1):c>>>1;crcT[n]=c>>>0;}
const crc=(b)=>{let c=0xffffffff;for(const x of b)c=crcT[(c^x)&0xff]^(c>>>8);return(c^0xffffffff)>>>0;};
const chunk=(t,d)=>{const l=Buffer.alloc(4);l.writeUInt32BE(d.length);const body=Buffer.concat([Buffer.from(t),d]);const c=Buffer.alloc(4);c.writeUInt32BE(crc(body));return Buffer.concat([l,body,c]);};
const ih=Buffer.alloc(13);ih.writeUInt32BE(W,0);ih.writeUInt32BE(H,4);ih[8]=8;ih[9]=2;
fs.writeFileSync(out,Buffer.concat([Buffer.from([0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a]),chunk('IHDR',ih),chunk('IDAT',zlib.deflateSync(raw)),chunk('IEND',Buffer.alloc(0))]));
