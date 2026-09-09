/*
 * ui-anim - the animation runtime for designs built in the BF6 UI BUILDER.
 *
 * Drop this file into your project beside the ParseUI export and call tick()
 * once per Events.OngoingGlobal callback (or on a utils Timers interval when a
 * slower rate is enough). It is plain mod.* calls and nothing else: no
 * dependency, no bundler step, and nothing that has to exist before it runs.
 *
 * THE THREE LAWS THIS RUNTIME KEEPS, because breaking any of them is what
 * makes a Portal HUD leak or flicker:
 *
 *   1. Widgets are created once. This file never calls AddUI* and never calls
 *      DeleteUIWidget. The ParseUI export builds the tree; this only writes to
 *      it. Show and hide go through SetUIWidgetVisible.
 *   2. Only changed properties are written. Every value written is remembered,
 *      and a tick that lands on the same number sends nothing. A sixty-tick
 *      hold costs one write, not sixty.
 *   3. Names are resolved once. FindUIWidgetWithName is a lookup, so it is
 *      done the first time a track needs a widget and cached from then on.
 *
 * Easing is the same maths the builder's canvas previewed, so what played in
 * the tool is what plays in the game.
 */

export type UiEase = 'linear' | 'easeIn' | 'easeOut' | 'easeInOut' | 'step';

/** A property this runtime knows how to write. */
export type UiProperty =
    | 'position' | 'size' | 'bgAlpha' | 'bgColor'
    | 'textColor' | 'textAlpha' | 'textSize' | 'padding'
    | 'imageAlpha' | 'visible';

export interface UiKey {
    /** Tick, counted from the moment the clip started. */
    t: number;
    /** A number for a scalar, [x, y] for a box, [r, g, b] for a colour. */
    v: number | number[] | boolean;
    /** How the value travels from this key to the next. */
    ease?: UiEase;
}

export interface UiTrack {
    /** The widget's name, as FindUIWidgetWithName wants it. */
    widget: string;
    property: UiProperty;
    keys: UiKey[];
}

export interface UiClip {
    name: string;
    /** Length in ticks. */
    duration: number;
    loop: boolean;
    tracks: UiTrack[];
}

/* ------------------------------------------------------------------ */

function shape(kind: UiEase | undefined, u: number): number {
    const t = u < 0 ? 0 : u > 1 ? 1 : u;
    switch (kind) {
        case 'step': return 0;
        case 'easeIn': return t * t;
        case 'easeOut': return 1 - (1 - t) * (1 - t);
        case 'easeInOut': return t * t * (3 - 2 * t);
        default: return t;
    }
}

function blend(a: UiKey['v'], b: UiKey['v'], u: number): number | number[] | boolean {
    if (typeof a === 'boolean' || typeof b === 'boolean') return u >= 1 ? b : a;
    if (Array.isArray(a) && Array.isArray(b)) {
        const out: number[] = [];
        const n = a.length > b.length ? a.length : b.length;
        for (let i = 0; i < n; i++) {
            const av = a[i] === undefined ? 0 : a[i];
            const bv = b[i] === undefined ? 0 : b[i];
            out.push(av + (bv - av) * u);
        }
        return out;
    }
    return (a as number) + ((b as number) - (a as number)) * u;
}

function sample(track: UiTrack, t: number): UiKey['v'] | undefined {
    const keys = track.keys;
    if (!keys || keys.length === 0) return undefined;
    if (t <= keys[0].t) return keys[0].v;
    const last = keys[keys.length - 1];
    if (t >= last.t) return last.v;
    for (let i = 0; i + 1 < keys.length; i++) {
        const k0 = keys[i];
        const k1 = keys[i + 1];
        if (t >= k0.t && t <= k1.t) {
            const span = k1.t - k0.t;
            const u = span <= 0 ? 1 : (t - k0.t) / span;
            return blend(k0.v, k1.v, shape(k0.ease, u));
        }
    }
    return last.v;
}

function same(a: unknown, b: unknown): boolean {
    if (a === b) return true;
    if (Array.isArray(a) && Array.isArray(b)) {
        if (a.length !== b.length) return false;
        for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */

interface Playing {
    clip: UiClip;
    frame: number;
    done: boolean;
}

export class UiAnim {
    private clips: Record<string, UiClip> = {};
    private playing: Playing[] = [];
    private widgets: Record<string, mod.UIWidget> = {};
    private written: Record<string, unknown> = {};

    constructor(clips: UiClip[]) {
        for (let i = 0; i < clips.length; i++) this.clips[clips[i].name] = clips[i];
    }

    /** Start a clip from tick zero. Starting one already playing restarts it. */
    play(name: string): void {
        const clip = this.clips[name];
        if (!clip) return;
        this.stop(name);
        this.playing.push({ clip: clip, frame: 0, done: false });
    }

    /** Stop a clip and leave every widget wherever it got to. */
    stop(name: string): void {
        const keep: Playing[] = [];
        for (let i = 0; i < this.playing.length; i++) {
            if (this.playing[i].clip.name !== name) keep.push(this.playing[i]);
        }
        this.playing = keep;
    }

    stopAll(): void { this.playing = []; }

    isPlaying(name: string): boolean {
        for (let i = 0; i < this.playing.length; i++) {
            if (this.playing[i].clip.name === name) return true;
        }
        return false;
    }

    /**
     * Advance every running clip by one tick and write what changed.
     * Call this from exactly one place. Two callers means double speed.
     */
    tick(): void {
        const still: Playing[] = [];
        for (let i = 0; i < this.playing.length; i++) {
            const p = this.playing[i];
            const dur = p.clip.duration > 0 ? p.clip.duration : 1;

            let t = p.frame;
            if (p.clip.loop) {
                t = t % dur;
                if (t < 0) t += dur;
            } else if (t >= dur) {
                t = dur;
                p.done = true;
            }

            const tracks = p.clip.tracks;
            for (let j = 0; j < tracks.length; j++) {
                const v = sample(tracks[j], t);
                if (v === undefined) continue;
                this.write(tracks[j].widget, tracks[j].property, v);
            }

            p.frame = p.frame + 1;
            if (!p.done) still.push(p);
        }
        this.playing = still;
    }

    /** Set a property once, outside any clip. Same change filter applies. */
    set(widget: string, property: UiProperty, value: number | number[] | boolean): void {
        this.write(widget, property, value);
    }

    /** Forget the cached lookups, after a teardown and a rebuild. */
    reset(): void {
        this.widgets = {};
        this.written = {};
        this.playing = [];
    }

    /* -------------------------------------------------------------- */

    private widget(name: string): mod.UIWidget | null {
        const cached = this.widgets[name];
        if (cached) return cached;
        const w = mod.FindUIWidgetWithName(name);
        if (!w) return null;
        this.widgets[name] = w;
        return w;
    }

    private write(name: string, property: UiProperty, value: number | number[] | boolean): void {
        const key = name + '.' + property;
        if (same(this.written[key], value)) return;

        const w = this.widget(name);
        if (!w) return;

        switch (property) {
            case 'position':
                mod.SetUIWidgetPosition(w, mod.CreateVector((value as number[])[0], (value as number[])[1], 0));
                break;
            case 'size':
                mod.SetUIWidgetSize(w, mod.CreateVector((value as number[])[0], (value as number[])[1], 0));
                break;
            case 'bgColor':
                mod.SetUIWidgetBgColor(w, mod.CreateVector(
                    (value as number[])[0], (value as number[])[1], (value as number[])[2]));
                break;
            case 'textColor':
                mod.SetUITextColor(w, mod.CreateVector(
                    (value as number[])[0], (value as number[])[1], (value as number[])[2]));
                break;
            case 'bgAlpha': mod.SetUIWidgetBgAlpha(w, value as number); break;
            case 'textAlpha': mod.SetUITextAlpha(w, value as number); break;
            case 'textSize': mod.SetUITextSize(w, value as number); break;
            case 'padding': mod.SetUIWidgetPadding(w, value as number); break;
            case 'imageAlpha': mod.SetUIImageAlpha(w, value as number); break;
            case 'visible': mod.SetUIWidgetVisible(w, value as boolean); break;
            default: return;
        }

        this.written[key] = Array.isArray(value) ? value.slice() : value;
    }
}

/*
 * USING IT
 *
 *   import { UiAnim } from './ui-anim';
 *   import { clips } from './my-design.anim';
 *   import './my-design';            // the ParseUI export builds the tree
 *
 *   const anim = new UiAnim(clips);
 *
 *   Events.OngoingGlobal.subscribe(() => { anim.tick(); });
 *   Events.OnPlayerDeployed.subscribe(() => { anim.play('bannerIn'); });
 *
 * WITHOUT THE EVENTS MODULE, on a plain Portal script, the same thing is an
 * Ongoing rule that calls anim.tick() once a tick.
 *
 * WITH SOLID-UI, an animation is a signal instead: keep the frame in a signal,
 * bind the property with an accessor, and let SolidUI decide when to write.
 * That is the better shape when the value comes from game state rather than
 * from a timeline. This runtime is for the timeline case.
 */
