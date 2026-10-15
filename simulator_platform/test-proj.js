const fs = require('fs');
const DEG = Math.PI / 180;
const N = (Math.cos(25*DEG) + Math.cos(47*DEG)) / 2;
const C = Math.cos(25*DEG)**2 + 2*N*Math.sin(25*DEG);
const R0 = Math.sqrt(C)/N;
const R_MIN = Math.sqrt(C - 2*N*Math.sin(54*DEG))/N;

function albers(lng, lat) {
  const phi = DEG * lat;
  const r = Math.sqrt(C - 2*N*Math.sin(phi)) / N;
  const theta = N * DEG * (lng - 105);
  const x = r * Math.sin(theta) + 500;
  const y = 22 + 680 * (R0 - r) / (R0 - R_MIN);
  return [x, y];
}

const geo = JSON.parse(fs.readFileSync('/app/web/geo/china-provinces.geojson', 'utf8'));
console.log('type:', geo.type, 'features:', geo.features.length);
const f = geo.features[0];
console.log('first geom type:', f.geometry.type);
let coords;
if (f.geometry.type === 'Polygon') {
  coords = f.geometry.coordinates[0][0];
} else if (f.geometry.type === 'MultiPolygon') {
  coords = f.geometry.coordinates[0][0][0];
}
console.log('first coord sample:', JSON.stringify(coords).slice(0,100));
const [x, y] = albers(coords[0], coords[1]);
console.log('projected to:', x.toFixed(1), y.toFixed(1), 'inView:', x>0&&x<1000&&y>0&&y<820);
