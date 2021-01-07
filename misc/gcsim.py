from random import choice
import random
from itertools import count
import sys
import time

sim_verbose = False

queue_g = []
queue_n = []

def mark_g(obj):
    if sim_verbose:
        print(f"Mark {obj} => G3 ({obj.props})")
    obj.gen = "G3"
    queue_g.append(obj)

def mark_n(obj):
    if sim_verbose:
        print(f"Mark {obj} => N4 ({obj.props})")
    obj.gen = "N4"
    queue_n.append(obj)

def walk_g(obj):
    for ref in obj.props.values():
        if ref.gen == "G2":
            mark_g(ref)
        elif ref.gen[0] == "N":
            mark_g(ref)
    if sim_verbose:
        print(f"Walk G {obj} {obj.props}")

def walk_n(obj):
    for ref in obj.props.values():
        if ref.gen == "G2":
            mark_g(ref)
        elif ref.gen == "N3":
            mark_n(ref)
    if sim_verbose:
        print(f"Walk N {obj} {obj.props}")

all_objs = []
scanning_stacks = False

class Object:
    def __init__(self, gen):
        all_objs.append(self)
        self.gen = gen
        self.id = len(all_objs)
        self.props = { }
        self.ok = False
    
    def __repr__(self):
        return f"{self.id}:{self.gen}"

    def set(self, name, obj):
        global sim_verbose

        prev = self.props.get(name)
        self.props[name] = obj

        if sim_verbose:
            print(f"{self}.{name} = {obj} ({prev})")

        if self.gen[0] == "G" and obj.gen[0] == "N":
            mark_g(obj)
        elif self.gen == "N4" and obj.gen == "N3":
            mark_n(obj)

        if prev and prev.gen == "G2":
            mark_g(prev)
        elif prev and prev.gen == "N3":
            mark_n(prev)

class Thread:
    def __init__(self, name):
        self.name = name
        self.stack = []
        self.alloc_gen = "N3"
    
    def __repr__(self):
        return self.name

root = Object("G3")
names = list("xyzw")

def thread_sim(func):
    return lambda thread: lambda: func(thread)

def sim_gc_g():
    if queue_g:
        walk_g(queue_g.pop())

def sim_gc_n():
    if queue_n:
        walk_n(queue_n.pop())

@thread_sim
def sim_new(t):
    t.stack.append(Object(t.alloc_gen))
    if sim_verbose:
        print(f".. {t} new: {t.stack}")

@thread_sim
def sim_dup(t):
    if not t.stack: return
    t.stack.append(t.stack[-1])
    if sim_verbose:
        print(f".. {t} dup: {t.stack}")

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
    + [sim_gc_g] * 2
    + [sim_gc_n] * 2
    + [sim_new(t) for t in threads]
    + [sim_dup(t) for t in threads] * 2
    + [sim_load(t) for t in threads]
    + [sim_store(t) for t in threads]
    + [sim_load_root(t) for t in threads]
    + [sim_store_root(t) for t in threads]
    )

sims_gc = (sims
    + [sim_gc_g] * 5
    + [sim_gc_n] * 5
)

def check_retain(work):
    while work:
        obj = work.pop()
        if obj.ok: continue
        if not (obj.gen == "G3" or obj.gen == "N4"):
            print(f"FAIL {obj}!")
            return False
        obj.ok = True
        for prop in obj.props.values():
            if obj.gen == "G3" and prop.gen != "G3":
                print(f"FAIL {obj} -> {prop}!")
                return False
            work.append(prop)
    return True

def mark_stack(t):
    t.alloc_gen = "G3"
    for s in t.stack:
        if s.gen != "G3":
            mark_g(s)
    if sim_verbose:
        print(f".. {t} mark stack: {t.stack}")

def simulate(seed, verbose):
    global root
    global sim_verbose
    global scanning_stacks

    all_objs.clear()
    random.seed(seed)
    sim_verbose = verbose
    next_id = 1
    scanning_stacks = False

    steps = random.choices(
        [100, 1000, 10000],
        [100, 10, 1],
    )[0]

    steps = 15

    root = Object("G3")
    for t in threads:
        t.stack.clear()
        t.alloc_gen = "N3"

    for n in range(steps):
        choice(sims)()

    scanning_stacks = True

    for t in threads:
        mark_stack(t)
        for n in range(steps):
            choice(sims)()

    while queue_g or queue_n:
        while queue_g or queue_n:
            choice(sims_gc)()

    roots = [root]
    for t in threads:
        roots += t.stack

    if verbose:
        for t in threads:
            print(t.stack)
        for obj in all_objs:
            print(f"{obj} => {obj.props}")
        assert not check_retain(roots)
        return True

    if not check_retain(roots):
        print(f"Fail at attempt {seed}")

        return simulate(seed, True)
    return False

t = 0

start = int(sys.argv[1] if len(sys.argv) > 1 else 0)
step = int(sys.argv[2] if len(sys.argv) > 2 else 100)
for ix in count():
    pt, t = t, int(time.time()) // 10
    n = start + ix * step
    if pt != t:
        print(n, flush=True)
    if simulate(n, False):
        break
