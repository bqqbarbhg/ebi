from random import choice
import random

sim_verbose = False

queue_g = []
queue_n = []

def mark_g(obj):
    if sim_verbose:
        print(f"Mark {obj} => G3")
    obj.gen = "G3"
    queue_g.append(obj)

def mark_n(obj):
    if sim_verbose:
        print(f"Mark {obj} => N4")
    obj.gen = "N4"
    queue_n.append(obj)

def walk_g(obj):
    for ref in obj.props.values():
        if ref.gen == "G2":
            mark_g(ref)
        elif ref.gen[0] == "N":
            mark_g(ref)

def walk_n(obj):
    for ref in obj.props.values():
        if ref.gen == "G2":
            mark_g(ref)
        elif ref.gen == "N3":
            mark_n(ref)

all_objs = []

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

        prev = self.props.get(obj)
        self.props[name] = obj

        if sim_verbose:
            print(f"{self}.{name} = {obj} ({prev})")

        if self.gen[0] == "G" and obj.gen[0] == "N":
            mark_g(obj)

        if prev and prev.gen == "G2":
            mark_g(prev)
        elif prev and prev.prev == "N3":
            mark_n(prev)

class Thread:
    def __init__(self):
        self.stack = []

root = Object("G3")
alloc_gen = "N3"
names = list("xyzw")

def thread_sim(func):
    return lambda thread: lambda: func(thread)

def sim_gc_g():
    if queue_g:
        walk_g(queue_g.pop())

def sim_gc_n():
    if queue_n:
        walk_n(queue_g.pop())

@thread_sim
def sim_new(t):
    t.stack.append(Object(alloc_gen))

@thread_sim
def sim_dup(t):
    if not t.stack: return
    t.stack.append(t.stack[-1])

@thread_sim
def sim_load(t):
    if not t.stack: return
    obj = t.stack.pop()
    if obj.props:
        t.stack.append(choice(list(obj.props.values())))

@thread_sim
def sim_store(t):
    if len(t.stack) < 2: return
    obj = t.stack.pop()
    t.stack[-1].set(choice(names), obj)

@thread_sim
def sim_load_root(t):
    if root.props:
        t.stack.append(choice(list(root.props.values())))

@thread_sim
def sim_store_root(t):
    if not t.stack: return
    obj = t.stack.pop()
    root.set(choice(names), obj)

threads = [Thread() for _ in range(4)]

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
            work.append(prop)
    return True

def mark_stacks(threads):
    for t in threads:
        for s in t.stack:
            if s.gen != "G3":
                mark_g(s)

def simulate(n, verbose):
    global root
    global alloc_gen
    global sim_verbose

    all_objs.clear()
    random.seed(n)
    sim_verbose = verbose
    next_id = 1

    root = Object("G3")
    for t in threads:
        t.stack.clear()

    alloc_gen = "N3"
    for n in range(200):
        choice(sims)()

    alloc_gen = "G3"
    mark_stacks(threads)
    while queue_g or queue_n:
        while queue_g or queue_n:
            choice(sims_gc)()
        mark_stacks(threads)
        
    if verbose:
        return True

    roots = [root]
    for t in threads:
        roots += t.stack
    if not check_retain(roots):
        print(f"Fail at attempt {n}")

        for t in threads:
            print(t.stack)
        for obj in all_objs:
            print(f"{obj}: {obj.props}")

        return simulate(n, True)
    return False

for n in range(1000000):
    if n % 1000 == 0:
        print(".", end="", flush=True)
    if simulate(n, False):
        break
