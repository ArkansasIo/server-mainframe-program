'use strict';

/**
 * A minimal fake DOM for the UI component tests.
 *
 * The components are deliberately written against a small, ordinary slice of
 * the DOM (createElement, appendChild, attributes, listeners, classList) so
 * they can be exercised in Node without a browser or a heavyweight DOM
 * emulation package. This module is that slice.
 *
 * It is a test double, not a browser: anything a component needs beyond this
 * list should be reconsidered, because the real dashboard would then depend on
 * behaviour the tests cannot see.
 */

class FakeNode {
  constructor(tag) {
    this.tagName = String(tag).toUpperCase();
    this.children = [];
    this.attributes = {};
    this.dataset = {};
    this.style = {};
    this.listeners = {};
    this.parentNode = null;
    this.className = '';
    this.textContent = '';
    this.hidden = false;
    this.value = '';
    this.scrollTop = 0;
    this.scrollHeight = 0;

    const node = this;
    this.classList = {
      add: (name) => {
        if (!node.classList.contains(name)) node.className = `${node.className} ${name}`.trim();
      },
      remove: (name) => {
        node.className = node.className.split(' ').filter((c) => c && c !== name).join(' ');
      },
      contains: (name) => node.className.split(' ').includes(name),
      toggle: (name) => (node.classList.contains(name)
        ? node.classList.remove(name)
        : node.classList.add(name)),
    };
  }

  appendChild(node) {
    node.parentNode = this;
    this.children.push(node);
    return node;
  }

  removeChild(node) {
    const index = this.children.indexOf(node);
    if (index >= 0) this.children.splice(index, 1);
    node.parentNode = null;
    return node;
  }

  get firstChild() { return this.children[0] || null; }

  setAttribute(key, value) { this.attributes[key] = String(value); }
  getAttribute(key) { return this.attributes[key]; }
  hasAttribute(key) { return Object.prototype.hasOwnProperty.call(this.attributes, key); }
  removeAttribute(key) { delete this.attributes[key]; }

  addEventListener(type, fn) { (this.listeners[type] || (this.listeners[type] = [])).push(fn); }

  removeEventListener(type, fn) {
    const list = this.listeners[type] || [];
    const index = list.indexOf(fn);
    if (index >= 0) list.splice(index, 1);
  }

  /** Fire a listener; this is how the tests "click" and "press keys". */
  fire(type, event = {}) {
    const payload = { target: this, preventDefault() { }, stopPropagation() { }, ...event };
    for (const fn of this.listeners[type] || []) fn(payload);
    return payload;
  }

  getBoundingClientRect() { return { left: 0, top: 0, width: 0, height: 0 }; }
  focus() { this.focused = true; }
  click() { this.fire('click'); }

  /** Depth-first text of every descendant, for cheap assertions. */
  get allText() {
    return this.children.map((c) => c.textContent + c.allText).join('');
  }

  find(predicate) {
    if (predicate(this)) return this;
    for (const child of this.children) {
      const hit = child.find ? child.find(predicate) : null;
      if (hit) return hit;
    }
    return null;
  }

  findAll(predicate, out = []) {
    if (predicate(this)) out.push(this);
    for (const child of this.children) {
      if (child.findAll) child.findAll(predicate, out);
    }
    return out;
  }
}

/** A document with just enough surface for the components. */
function fakeDocument() {
  const doc = new FakeNode('document');
  doc.createElement = (tag) => new FakeNode(tag);
  doc.createTextNode = (text) => {
    const node = new FakeNode('#text');
    node.textContent = text;
    return node;
  };
  doc.body = new FakeNode('body');
  doc.readyState = 'complete';
  return doc;
}

/** Matches a node whose class attribute contains `name`. */
const withClass = (name) => (node) => String(node.className).split(' ').includes(name);

module.exports = { FakeNode, fakeDocument, withClass };
