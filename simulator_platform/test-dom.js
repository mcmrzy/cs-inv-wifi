const fs = require('fs');
const { JSDOM } = require('jsdom');

// Setup DOM
const dom = new JSDOM('<!DOCTYPE html><html><body><svg id="china" viewBox="0 0 1000 820"></svg></body></html>');
global.document = dom.window.document;
global.window = dom.window;
global.HTMLElement = dom.window.HTMLElement;

// Now eval the map.js
const mapCode = fs.readFileSync('/app/web/js/map.js', 'utf8');
eval(mapCode);

// Check what's in the SVG
const svg = document.getElementById('china');
console.log('SVG children:', svg.children.length);
console.log('SVG innerHTML length:', svg.innerHTML.length);
console.log('First 300 chars of SVG:', svg.innerHTML.slice(0,300));

// Check if content group exists
const groups = svg.querySelectorAll('g');
console.log('g elements:', groups.length);
if (groups.length > 0) console.log('First g class:', groups[0].getAttribute('class'));
if (groups.length > 1) console.log('Second g class:', groups[1].getAttribute('class'));
