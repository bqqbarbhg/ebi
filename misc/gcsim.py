from random import choice
import random
from itertools import count, product
import sys
import time
import string
from collections import namedtuple

Epoch = namedtuple("Epoch", "g n")

sim_verbose = False

queue_n = []

def mark_ng(epoch, obj):
    if obj.g == epoch.g: return
    obj.g = epoch.g
    if sim_verbose:
        print(f"Mark NG {obj} ({obj.props})")
    queue_n.append(obj)

def mark_nn(epoch, obj):
    if obj.g:
        mark_ng(epoch, obj)
        return
    if obj.n == epoch.n: return
    obj.n = epoch.n
    if sim_verbose:
        print(f"Mark NN {obj} ({obj.props})")
    queue_n.append(obj)

def walk_n(epoch, obj):
    if obj.g:
        for ref in obj.props.values():
            mark_ng(epoch, ref)
        if sim_verbose:
            print(f"Walk NG {obj} {obj.props}")
    else:
        for ref in obj.props.values():
            mark_nn(epoch, ref)
        if sim_verbose:
            print(f"Walk NN {obj} {obj.props}")

all_objs = []
name_iter = ("".join(s) for s in product(string.ascii_uppercase, repeat=3))

class Object:
    def __init__(self, epoch, alloc_g):
        self.g = epoch.g if alloc_g else 0
        self.n = epoch.n
        self.id = next(name_iter)
        self.props = { }
        all_objs.append(self)

    def __repr__(self):
        return f"{self.id}:{self.g}:{self.n}"

    def set(self, epoch, name, obj):
        global sim_verbose

        prev = self.props.get(name)
        self.props[name] = obj

        if sim_verbose:
            print(f"{self}.{name} = {obj} ({prev})")
        
        if prev:
            mark_nn(epoch, prev)
        
        if self.g ^ obj.g:
            mark_ng(epoch, obj)
        elif not obj.g:
            mark_nn(epoch, obj)

class Thread:
    def __init__(self, name):
        self.name = name
        self.stack = []
        self.alloc_g = False
        self.epoch = Epoch(0,0)
    
    def __repr__(self):
        return f"{self.name}:{self.epoch.g}:{self.epoch.n}"

root = Object(Epoch(0, 0), True)
names = list("xy")

def thread_sim(func):
    def sim_thread(t):
        def inner():
            func(t)
            if len(t.stack) > 20:
                t.stack = t.stack[-20:]
                if sim_verbose:
                    print(f".. {t} truncate: {t.stack}")
        return inner
    return sim_thread

@thread_sim
def sim_gc_n(t):
    if queue_n:
        walk_n(t.epoch, queue_n.pop())

@thread_sim
def sim_new(t):
    t.stack.append(Object(t.epoch, t.alloc_g))
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
    t.stack[-1].set(t.epoch, choice(names), obj)
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
    root.set(t.epoch, choice(names), obj)
    if sim_verbose:
        print(f".. {t} store root: {root.props}")

threads = [Thread(f"t{n}") for n in range(4)]

sims = ([]
    + [sim_gc_n(t) for t in threads]
    + [sim_new(t) for t in threads]
    + [sim_dup(t) for t in threads]
    + [sim_pop(t) for t in threads]
    + [sim_load(t) for t in threads]
    + [sim_store(t) for t in threads]
    + [sim_load_root(t) for t in threads]
    + [sim_store_root(t) for t in threads]
    )

sims_gc = (sims
    + [sim_gc_n(t) for t in threads] * 4
)

def alive(epoch, obj):
    return obj.g == epoch.g or (obj.g == 0 and obj.n == epoch.n)

def check(epoch, roots):
    all_set = set(all_objs)
    ok = set(roots)
    work = list(ok)
    while work:
        obj = work.pop()
        if not alive(epoch, obj):
            print(f"Bad epoch at {epoch.g}:{epoch.n}  {roots[obj]} => {obj}")
            return False
        if obj not in all_set:
            print(f"Dead object at {epoch.g}:{epoch.n}  {roots[obj]} => {obj}")
            return False
        for key, prop in obj.props.items():
            if obj.g and not prop.g:
                print(f"G to N link {epoch.g}:{epoch.n}  {obj}[{key}] => {prop}")
                return False
            if prop in ok: continue
            ok.add(prop)
            work.append(prop)
            roots[prop] = f"{roots[obj]}.{key}"
    return True

def mark_stack(epoch, t):
    for s in t.stack:
        mark_nn(epoch, s)
    if sim_verbose:
        print(f".. {t} mark stack {epoch.g}:{epoch.n} => {t.stack}")

def simulate(seed, verbose):
    global root
    global sim_verbose
    global name_iter
    global all_objs

    name_iter = ("".join(s) for s in product(string.ascii_uppercase, repeat=3))
    epoch = Epoch(1, 1)

    all_objs.clear()
    random.seed(seed)
    sim_verbose = verbose
    next_id = 1

    steps = 30
    steps_thread = 10

    root = Object(epoch, True)

    for t in threads:
        t.stack.clear()
        t.epoch = epoch

    if verbose:
        print(f"Seed: {seed}")

    for cycle in range(100):
        if verbose:
            print(f"==== Start {cycle} ====")

        for t in threads:
            t.epoch = epoch
            t.alloc_g = False

        for t in threads:
            mark_stack(epoch, t)
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

        all_objs = [o for o in all_objs if alive(epoch, o)]

        if verbose:
            print(f"==== Roots at {cycle} ====")
            print(f"root => {root} {root.props}")
            for t in threads:
                print(f"{t} => {t.stack}")
            print(f"==== Alive at {cycle} ====")
            for obj in all_objs:
                print(f"{obj} => {obj.props}")

        if not check(epoch, roots):
            if verbose: return False
            return simulate(seed, True)

        epoch = Epoch(epoch.g, epoch.n + 1)
        if verbose:
            print(f"GC: N at {epoch.g}:{epoch.n}")

        if cycle % 10 == 0:
            epoch = Epoch(epoch.g + 1, epoch.n)
            if verbose:
                print(f"GC: G at {epoch.g}:{epoch.n}")

            mark_ng(epoch, root)
            for t in threads:
                for s in t.stack:
                    mark_ng(epoch, s)

    return True

t = 0

start = int(sys.argv[1] if len(sys.argv) > 1 else 0)
step = int(sys.argv[2] if len(sys.argv) > 2 else 100)
for ix in count():
    pt, t = t, int(time.time()) // 10
    n = start + ix * step
    if pt != t or n < 100:
        print(n, flush=True)
    if not simulate(n, False):
        break
