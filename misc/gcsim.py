from random import choice
import random
from itertools import count, product
import sys
import time
import string

sim_verbose = False

queue_g = []
queue_n = []

epoch_g = 3
epoch_n = 3

def mark_gg(obj):
    if obj.g == epoch_g: return
    if sim_verbose:
        print(f"Mark GG {obj} ({obj.props})")
    obj.g = epoch_g
    queue_g.append(obj)

def mark_ng(obj):
    if obj.g == epoch_g: return
    if sim_verbose:
        print(f"Mark NG {obj} ({obj.props})")
    obj.g = epoch_g
    queue_n.append(obj)

def mark_nn(obj):
    if obj.g: return
    if obj.n == epoch_n: return
    if sim_verbose:
        print(f"Mark NN {obj} ({obj.props})")
    obj.n = epoch_n
    queue_n.append(obj)

def walk_g(obj):
    for ref in obj.props.values():
        mark_gg(ref)
    if sim_verbose:
        print(f"Walk G {obj} {obj.props}")

def walk_n(obj):
    if obj.g:
        for ref in obj.props.values():
            mark_ng(ref)
        if sim_verbose:
            print(f"Walk NG {obj} {obj.props}")
    else:
        for ref in obj.props.values():
            if ref.g: continue
            mark_nn(ref)
        if sim_verbose:
            print(f"Walk NN {obj} {obj.props}")

all_objs = []
name_iter = ("".join(s) for s in product(string.ascii_uppercase, repeat=3))

class Object:
    def __init__(self, alloc_g):
        self.g = epoch_g if alloc_g else 0
        self.n = epoch_n
        self.id = next(name_iter)
        self.props = { }
        all_objs.append(self)

    def __repr__(self):
        return f"{self.id}:{self.g}:{self.n}"

    def set(self, name, obj):
        global sim_verbose

        prev = self.props.get(name)
        self.props[name] = obj

        if sim_verbose:
            print(f"{self}.{name} = {obj} ({prev})")
        
        if prev:
            if prev.g:
                mark_gg(prev)
            else:
                mark_nn(prev)

        if not obj.g:
            if self.g:
                mark_ng(obj)
            else:
                mark_nn(obj)

class Thread:
    def __init__(self, name):
        self.name = name
        self.stack = []
        self.alloc_g = False
    
    def __repr__(self):
        return self.name

root = Object(True)
names = list("xy")

def thread_sim(func):
    def sim_thread(t):
        def inner():
            func(t)
            if len(t.stack) > 10:
                t.stack = t.stack[-10:]
                if sim_verbose:
                    print(f".. {t} truncate: {t.stack}")
        return inner
    return sim_thread

def sim_gc_g():
    if queue_g:
        walk_g(queue_g.pop())

def sim_gc_n():
    if queue_n:
        walk_n(queue_n.pop())

@thread_sim
def sim_new(t):
    t.stack.append(Object(t.alloc_g))
    if sim_verbose:
        print(f".. {t} new: {t.stack}")

@thread_sim
def sim_dup(t):
    if not t.stack: return
    t.stack.append(t.stack[-1])
    if sim_verbose:
        print(f".. {t} dup: {t.stack}")

@thread_sim
def sim_pop(t):
    if not t.stack: return
    t.stack.pop()
    if sim_verbose:
        print(f".. {t} pop: {t.stack}")

@thread_sim
def sim_load(t):
    if not t.stack: return
    obj = t.stack.pop()
    if obj.props:
        t.stack.append(choice(list(obj.props.values())))
    if sim_verbose:
        print(f".. {t} load: {t.stack}")

@thread_sim
def sim_store(t):
    if len(t.stack) < 2: return
    obj = t.stack.pop()
    t.stack[-1].set(choice(names), obj)
    if sim_verbose:
        print(f".. {t} store: {t.stack[-1]} {t.stack[-1].props}")

@thread_sim
def sim_load_root(t):
    if root.props:
        t.stack.append(choice(list(root.props.values())))
        if sim_verbose:
            print(f".. {t} load root: {t.stack}")

@thread_sim
def sim_store_root(t):
    if not t.stack: return
    obj = t.stack.pop()
    root.set(choice(names), obj)
    if sim_verbose:
        print(f".. {t} store root: {root.props}")

threads = [Thread(f"t{n}") for n in range(4)]

sims = ([]
    + [sim_gc_g] * 1
    + [sim_gc_n] * 2
    + [sim_new(t) for t in threads]
    + [sim_dup(t) for t in threads]
    + [sim_pop(t) for t in threads]
    + [sim_load(t) for t in threads]
    + [sim_store(t) for t in threads]
    + [sim_load_root(t) for t in threads]
    + [sim_store_root(t) for t in threads]
    )

sims_gc = (sims
    + [sim_gc_g] * 1
    + [sim_gc_n] * 10
)

def alive(obj, use_g):
    if use_g:
        return obj.g == epoch_g or (obj.g == 0 and obj.n == epoch_n)
    else:
        return obj.g or obj.n == epoch_n

def check(roots, use_g):
    all_set = set(all_objs)
    ok = set(roots)
    work = list(ok)
    while work:
        obj = work.pop()
        ug = "NG"[use_g]
        if not alive(obj, use_g):
            print(f"Bad epoch at {ug} {epoch_g}:{epoch_n}  {roots[obj]} => {obj}")
            return False
        if obj not in all_set:
            print(f"Dead object at {ug} {epoch_g}:{epoch_n}  {roots[obj]} => {obj}")
            return False
        for key, prop in obj.props.items():
            if prop in ok: continue
            ok.add(prop)
            work.append(prop)
            roots[prop] = f"{roots[obj]}.{key}"
    return True

def mark_stack(t):
    for s in t.stack:
        mark_nn(s)
    if sim_verbose:
        print(f".. {t} mark stack {epoch_g}:{epoch_n} => {t.stack}")

def simulate(seed, verbose):
    global root
    global sim_verbose
    global name_iter
    global epoch_g
    global epoch_n
    global all_objs

    name_iter = ("".join(s) for s in product(string.ascii_uppercase, repeat=3))

    all_objs.clear()
    random.seed(seed)
    sim_verbose = verbose
    next_id = 1

    steps = 30
    steps_thread = 10

    root = Object(True)

    for t in threads:
        t.stack.clear()

    if verbose:
        print(f"Seed: {seed}")

    for cycle in range(100):
        if verbose:
            print(f"==== Start {cycle} ====")

        for t in threads:
            t.alloc_g = False

        for t in threads:
            mark_stack(t)
            for n in range(steps_thread):
                choice(sims)()

        for n in range(steps):
            choice(sims)()

        if verbose:
            print(f"==== GC {cycle} ====")

        for t in threads:
            t.alloc_g = True

        while queue_n:
            choice(sims_gc)()

        roots = { root: "root" }
        for t in threads:
            for i,s in enumerate(t.stack):
                if s in roots: continue
                roots[s] = f"{t}[{i}]"

        use_g = not queue_g and cycle % 2 == 0
        all_objs = [o for o in all_objs if alive(o, use_g)]

        if verbose:
            print(f"==== Roots at {cycle} ====")
            print(f"root => {root} {root.props}")
            for t in threads:
                print(f"{t} => {t.stack}")
            print(f"==== Alive at {cycle} ====")
            for obj in all_objs:
                print(f"{obj} => {obj.props}")

        if not check(roots, use_g):
            if verbose: return False
            return simulate(seed, True)

        epoch_n += 1
        if verbose:
            print(f"GC: N at {epoch_g}:{epoch_n}")

        if use_g:
            epoch_g += 1
            if verbose:
                print(f"GC: G at {epoch_g}:{epoch_n}")
            mark_gg(root)
            for t in threads:
                for s in t.stack:
                    mark_ng(s)

    return True

simulate(0, True)
