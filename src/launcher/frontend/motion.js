"use strict";

window.LauncherMotion = (() => {
  const preference = matchMedia("(prefers-reduced-motion: reduce)");
  const panels = new Set(), indicators = new Set(), effects = new Set();
  function token(name) { return getComputedStyle(document.documentElement).getPropertyValue("--motion-" + name).trim(); }
  function timing(name) { return {duration:parseFloat(token(name)),easing:token("ease"),fill:"both"}; }
  function quiet() { return preference.matches || document.hidden; }
  function cancelEffects(root) {
    for (const animation of effects) if (root.contains(animation.effect.target)) animation.cancel();
  }
  function effect(element, frames, name, delay = 0) {
    if (quiet()) return;
    const animation = element.animate(frames, {...timing(name),delay});
    effects.add(animation);
    animation.finished.catch(() => {}).finally(() => { effects.delete(animation); animation.cancel(); });
  }

  class PanelTransition {
    constructor(container, entries, current, onShow, onSettled) {
      this.container = container;
      this.entries = new Map(entries);
      this.current = this.target = current;
      this.state = "idle";
      this.generation = 0;
      this.animation = null;
      this.onShow = onShow;
      this.onSettled = onSettled;
      this.finished = Promise.resolve();
      panels.add(this);
    }
    stateChanged(value) { this.state = value; this.container.dataset.motionState = value; }
    cancel(preserve) {
      if (!this.animation) return;
      if (preserve) { try { this.animation.commitStyles(); } catch {} }
      this.animation.cancel();
      this.animation = null;
    }
    visibility(key) {
      for (const [name, panel] of this.entries) {
        const active = key === name;
        panel.hidden = !active;
        panel.inert = !active;
        panel.setAttribute("aria-hidden", String(!active));
        panel.style.removeProperty("opacity");
        panel.style.removeProperty("transform");
        if (!active) cancelEffects(panel);
      }
    }
    snap(key) {
      const changed = this.current !== key || (key !== null && this.entries.get(key).hidden);
      ++this.generation;
      this.cancel(false);
      this.current = this.target = key;
      this.visibility(key);
      this.stateChanged("idle");
      if (changed && key !== null) this.onShow(key);
      this.onSettled();
      this.finished = Promise.resolve();
      return this.finished;
    }
    finish() { return this.snap(this.target); }
    switchTo(key, immediate = false) {
      if (key === this.target && this.state === "idle") return this.finished;
      if (immediate || quiet()) return this.snap(key);
      const generation = ++this.generation;
      this.target = key;
      this.cancel(true);
      this.finished = this.transition(key, generation);
      return this.finished;
    }
    async play(panel, frames, name) {
      const animation = panel.animate(frames, timing(name));
      this.animation = animation;
      await animation.finished.catch(() => {});
      return animation;
    }
    async transition(key, generation) {
      let panel = this.entries.get(this.current);
      if (panel && this.current !== key) {
        cancelEffects(panel);
        panel.inert = true;
        panel.setAttribute("aria-hidden", "true");
        this.stateChanged("exiting");
        const style = getComputedStyle(panel);
        await this.play(panel, [{opacity:style.opacity,transform:style.transform}, {opacity:0,transform:"translateY(-6px)"}], "exit");
        if (generation !== this.generation) return;
        this.cancel(false);
        panel.hidden = true;
        this.current = null;
      }
      if (generation !== this.generation) return;
      if (key === null) { this.snap(null); return; }
      panel = this.entries.get(key);
      const continuing = this.current === key && !panel.hidden;
      const previous = continuing ? getComputedStyle(panel) : null;
      const start = continuing ? {opacity:previous.opacity,transform:previous.transform} : {opacity:0,transform:"translateY(8px)"};
      this.current = key;
      for (const [name, other] of this.entries) if (name !== key) {
        other.hidden = other.inert = true;
        other.setAttribute("aria-hidden", "true");
        cancelEffects(other);
      }
      panel.hidden = panel.inert = false;
      panel.setAttribute("aria-hidden", "false");
      this.stateChanged("entering");
      if (!continuing) this.onShow(key);
      await this.play(panel, [start, {opacity:1,transform:"translateY(0)"}], "normal");
      if (generation !== this.generation) return;
      this.cancel(false);
      panel.style.removeProperty("opacity");
      panel.style.removeProperty("transform");
      this.stateChanged("idle");
      this.onSettled();
    }
  }

  class SlidingIndicator {
    constructor(bar, element, items, selected, vertical = false) {
      Object.assign(this, {bar,element,items,selected,vertical,frame:0,animation:null,ready:false,pendingAnimate:false});
      this.observer = new ResizeObserver(() => this.schedule(false));
      for (const item of [bar,...items]) this.observer.observe(item);
      indicators.add(this);
    }
    schedule(animate) {
      this.pendingAnimate = this.frame ? this.pendingAnimate && animate : animate;
      if (this.frame) return;
      this.frame = requestAnimationFrame(() => { this.frame = 0; this.move(this.pendingAnimate); });
    }
    move(animate) {
      const selected = this.selected(), rect = selected?.getBoundingClientRect(), bar = this.bar.getBoundingClientRect();
      if (!rect?.width || !bar.width) { this.ready = false; this.element.style.opacity = "0"; this.cancel(); return; }
      const old = getComputedStyle(this.element);
      const from = {transform:old.transform,width:old.width,height:old.height};
      this.cancel();
      const target = {transform:`translate(${rect.left - bar.left}px,${this.vertical ? rect.top - bar.top : 0}px)`,width:`${rect.width}px`,height:this.vertical ? `${rect.height}px` : "2px"};
      Object.assign(this.element.style, target, {opacity:"1"});
      if (animate && this.ready && !quiet()) {
        const animation = this.element.animate([from,target],timing("indicator"));
        this.animation = animation;
        animation.finished.catch(() => {}).finally(() => {
          if (this.animation === animation) this.animation = null;
          animation.cancel();
        });
      }
      this.ready = true;
    }
    cancel() {
      if (this.animation) this.animation.cancel();
      this.animation = null;
    }
    finish() { cancelAnimationFrame(this.frame); this.frame = 0; this.move(false); }
    dispose() { this.observer.disconnect(); cancelAnimationFrame(this.frame); this.cancel(); }
  }

  function finish() {
    for (const controller of panels) controller.finish();
    for (const indicator of indicators) indicator.finish();
    for (const animation of effects) animation.cancel();
  }
  preference.addEventListener("change", () => { if (preference.matches) finish(); });
  document.addEventListener("visibilitychange", () => { if (document.hidden) finish(); });
  window.addEventListener("pagehide", () => { finish(); for (const indicator of indicators) indicator.dispose(); });
  return {PanelTransition,SlidingIndicator,effect,cancelEffects,finish,preference};
})();
