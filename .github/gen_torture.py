#!/usr/bin/env python3
# Generator for .github/test.torture.c — the defer/orelse semantic torture tier.
#
# VALID units are emitted twice from one AST:
#   tor_N(sel)  — Prism source using defer/orelse
#   ref_N(sel)  — plain C twin; cleanup placed at every exit by THIS script's
#                 own scope-walker (an independent lowering of the semantics)
# Per (unit, sel) the harness asserts:
#   1. stack discipline: every defer fires LIFO, exactly once, none leaked
#   2. trace equality:   tor fire-sequence (ids AND captured values) == ref
#   3. return equality:  tor return value == ref return value
#   4. eval parity:      per-value-fn evaluation counts identical
# MISUSE units are C sources that must be REJECTED pre-emit; the runner feeds
# them through prism_transpile_source and asserts status != PRISM_OK.
#
# Deterministic: shapes come from a fixed seed. Regenerate with:
#   python3 .github/gen_torture.py > .github/test.torture.c

import random

rng = random.Random(0xDEFE7)

units = []
uid = 0
SELS = 4

class Ctx:
    def __init__(self):
        self.ind = 1
        self.out = []
    def w(self, s):
        self.out.append('\t' * self.ind + s)

# ---------------------------------------------------------------- AST nodes
# ('seq',[..]) ('block',[..]) ('defer',id) ('deferblk',id) ('deferv',id,var)
# ('ret',expr) ('if',cond,[..],[..]|None) ('for',n,[..]) ('while',n,[..])
# ('do',n,[..]) ('switch',[body0,body1,..]) ('brk',) ('cont',)
# ('oe_decl',v,fid,fb) ('oe_assign',v,fid,fb) ('oe_chain',v,f1,f2,fb)
# ('oe_chain3',v,f1,f2,f3,fb) ('oe_ret',v,fid,rv) ('oe_brk',v,fid)
# ('oe_cont',v,fid) ('oe_goto',v,fid) ('oe_blk',v,fid,[..]) ('stmt',code)
# ('mut',var,expr) ('stmtexpr',v,[..],k) ('goto_tail',)

def emit_prism(n, c):
    k = n[0]
    if k == 'seq':
        for x in n[1]:
            emit_prism(x, c)
    elif k == 'block':
        c.w('{')
        c.ind += 1
        for x in n[1]:
            emit_prism(x, c)
        c.ind -= 1
        c.w('}')
    elif k == 'defer':
        c.w('tt_acquire(%d);' % n[1])
        c.w('defer tt_release(%d);' % n[1])
    elif k == 'deferblk':
        c.w('tt_acquire(%d);' % n[1])
        c.w('defer { tt_touch(); tt_release(%d); }' % n[1])
    elif k == 'deferv':
        c.w('tt_acquire(%d);' % n[1])
        c.w('defer tt_release_val(%d, %s);' % (n[1], n[2]))
    elif k == 'ret':
        c.w('return %s;' % n[1])
    elif k == 'if':
        c.w('if (%s) {' % n[1])
        c.ind += 1
        for x in n[2]:
            emit_prism(x, c)
        c.ind -= 1
        if n[3] is not None:
            c.w('} else {')
            c.ind += 1
            for x in n[3]:
                emit_prism(x, c)
            c.ind -= 1
        c.w('}')
    elif k == 'for':
        c.w('for (int i%d = 0; i%d < %d; i%d++) {' % (c.ind, c.ind, n[1], c.ind))
        c.ind += 1
        for x in n[2]:
            emit_prism(x, c)
        c.ind -= 1
        c.w('}')
    elif k in ('while', 'do'):
        c.w('{')
        c.ind += 1
        c.w('int w%d = 0;' % c.ind)
        if k == 'while':
            c.w('while (w%d < %d) {' % (c.ind, n[1]))
        else:
            c.w('do {')
        c.ind += 1
        c.w('w%d++;' % (c.ind - 1))
        for x in n[2]:
            emit_prism(x, c)
        c.ind -= 1
        if k == 'while':
            c.w('}')
        else:
            c.w('} while (w%d < %d);' % (c.ind, n[1]))
        c.ind -= 1
        c.w('}')
    elif k == 'switch':
        c.w('switch (sel %% %d) {' % len(n[1]))
        for ci, body in enumerate(n[1]):
            c.w('case %d: {' % ci)
            c.ind += 1
            for x in body:
                emit_prism(x, c)
            c.ind -= 1
            c.w('} break;')
        c.w('}')
    elif k == 'brk':
        c.w('break;')
    elif k == 'cont':
        c.w('continue;')
    elif k == 'oe_decl':
        c.w('int %s = tv(%d, sel) orelse %d;' % (n[1], n[2], n[3]))
        c.w('acc += %s;' % n[1])
    elif k == 'oe_assign':
        c.w('%s = tv(%d, sel) orelse %d;' % (n[1], n[2], n[3]))
        c.w('acc += %s;' % n[1])
    elif k == 'oe_chain':
        c.w('int %s = tv(%d, sel) orelse tv(%d, sel) orelse %d;' % (n[1], n[2], n[3], n[4]))
        c.w('acc += %s;' % n[1])
    elif k == 'oe_chain3':
        c.w('int %s = tv(%d, sel) orelse tv(%d, sel) orelse tv(%d, sel) orelse %d;'
            % (n[1], n[2], n[3], n[4], n[5]))
        c.w('acc += %s;' % n[1])
    elif k == 'oe_ret':
        c.w('int %s = tv(%d, sel) orelse return %d;' % (n[1], n[2], n[3]))
        c.w('acc += %s;' % n[1])
    elif k == 'oe_brk':
        c.w('int %s = tv(%d, sel) orelse break;' % (n[1], n[2]))
        c.w('acc += %s;' % n[1])
    elif k == 'oe_cont':
        c.w('int %s = tv(%d, sel) orelse continue;' % (n[1], n[2]))
        c.w('acc += %s;' % n[1])
    elif k == 'oe_goto':
        c.w('int %s = tv(%d, sel) orelse goto tail;' % (n[1], n[2]))
        c.w('acc += %s;' % n[1])
    elif k == 'oe_blk':
        c.w('int %s = tv(%d, sel) orelse {' % (n[1], n[2]))
        c.ind += 1
        for x in n[3]:
            emit_prism(x, c)
        c.ind -= 1
        c.w('}')
        c.w('acc += %s;' % n[1])
    elif k == 'stmt':
        c.w(n[1])
    elif k == 'mut':
        c.w('%s = %s;' % (n[1], n[2]))
    elif k == 'stmtexpr':
        c.w('int %s = ({' % n[1])
        c.ind += 1
        for x in n[2]:
            emit_prism(x, c)
        c.w('%d;' % n[3])
        c.ind -= 1
        c.w('});')
        c.w('acc += %s;' % n[1])
    elif k == 'goto_tail':
        c.w('goto tail;')
    else:
        raise Exception('prism emit: ' + k)

# ------------------------------------------------- independent twin lowering
# scopes: list of lists of ('id', n) | ('val', id, var) — release order source.

def emit_ref(n, c, scopes, loops):
    k = n[0]
    def rel_one(e):
        if e[0] == 'id':
            c.w('tw_release(%d);' % e[1])
        else:
            c.w('tw_release_val(%d, %s);' % (e[1], e[2]))
    def release_scopes(upto):
        for s in reversed(scopes[upto:]):
            for e in reversed(s):
                rel_one(e)
    def close_scope():
        for e in reversed(scopes[-1]):
            rel_one(e)
        scopes.pop()
    if k == 'seq':
        for x in n[1]:
            emit_ref(x, c, scopes, loops)
    elif k == 'block':
        c.w('{')
        c.ind += 1
        scopes.append([])
        for x in n[1]:
            emit_ref(x, c, scopes, loops)
        close_scope()
        c.ind -= 1
        c.w('}')
    elif k in ('defer', 'deferblk'):
        c.w('tw_acquire(%d);' % n[1])
        scopes[-1].append(('id', n[1]))
    elif k == 'deferv':
        c.w('tw_acquire(%d);' % n[1])
        scopes[-1].append(('val', n[1], n[2]))
    elif k == 'ret':
        release_scopes(0)
        c.w('return %s;' % n[1])
    elif k == 'if':
        c.w('if (%s) {' % n[1])
        c.ind += 1
        scopes.append([])
        for x in n[2]:
            emit_ref(x, c, scopes, loops)
        close_scope()
        c.ind -= 1
        if n[3] is not None:
            c.w('} else {')
            c.ind += 1
            scopes.append([])
            for x in n[3]:
                emit_ref(x, c, scopes, loops)
            close_scope()
            c.ind -= 1
        c.w('}')
    elif k == 'for':
        c.w('for (int i%d = 0; i%d < %d; i%d++) {' % (c.ind, c.ind, n[1], c.ind))
        c.ind += 1
        scopes.append([])
        loops.append(('loop', len(scopes) - 1))
        for x in n[2]:
            emit_ref(x, c, scopes, loops)
        close_scope()
        loops.pop()
        c.ind -= 1
        c.w('}')
    elif k in ('while', 'do'):
        c.w('{')
        c.ind += 1
        scopes.append([])
        c.w('int w%d = 0;' % c.ind)
        if k == 'while':
            c.w('while (w%d < %d) {' % (c.ind, n[1]))
        else:
            c.w('do {')
        c.ind += 1
        scopes.append([])
        loops.append(('loop', len(scopes) - 1))
        c.w('w%d++;' % (c.ind - 1))
        for x in n[2]:
            emit_ref(x, c, scopes, loops)
        close_scope()
        loops.pop()
        c.ind -= 1
        if k == 'while':
            c.w('}')
        else:
            c.w('} while (w%d < %d);' % (c.ind, n[1]))
        close_scope()
        c.ind -= 1
        c.w('}')
    elif k == 'switch':
        # C break targets the nearest enclosing loop OR switch; each case
        # scope joins the breakable stack as a 'switch' entry (continue
        # still skips it and targets the nearest 'loop' entry).
        c.w('switch (sel %% %d) {' % len(n[1]))
        for ci, body in enumerate(n[1]):
            c.w('case %d: {' % ci)
            c.ind += 1
            scopes.append([])
            loops.append(('switch', len(scopes) - 1))
            for x in body:
                emit_ref(x, c, scopes, loops)
            loops.pop()
            close_scope()
            c.ind -= 1
            c.w('} break;')
        c.w('}')
    elif k == 'brk':
        release_scopes(loops[-1][1])
        c.w('break;')
    elif k == 'cont':
        release_scopes(next(l[1] for l in reversed(loops) if l[0] == 'loop'))
        c.w('continue;')
    elif k == 'oe_decl':
        c.w('int %s = tv(%d, sel);' % (n[1], n[2]))
        c.w('if (!%s) %s = %d;' % (n[1], n[1], n[3]))
        c.w('acc += %s;' % n[1])
    elif k == 'oe_assign':
        c.w('%s = tv(%d, sel);' % (n[1], n[2]))
        c.w('if (!%s) %s = %d;' % (n[1], n[1], n[3]))
        c.w('acc += %s;' % n[1])
    elif k == 'oe_chain':
        c.w('int %s = tv(%d, sel);' % (n[1], n[2]))
        c.w('if (!%s) %s = tv(%d, sel);' % (n[1], n[1], n[3]))
        c.w('if (!%s) %s = %d;' % (n[1], n[1], n[4]))
        c.w('acc += %s;' % n[1])
    elif k == 'oe_chain3':
        c.w('int %s = tv(%d, sel);' % (n[1], n[2]))
        c.w('if (!%s) %s = tv(%d, sel);' % (n[1], n[1], n[3]))
        c.w('if (!%s) %s = tv(%d, sel);' % (n[1], n[1], n[4]))
        c.w('if (!%s) %s = %d;' % (n[1], n[1], n[5]))
        c.w('acc += %s;' % n[1])
    elif k == 'oe_ret':
        c.w('int %s = tv(%d, sel);' % (n[1], n[2]))
        c.w('if (!%s) {' % n[1])
        c.ind += 1
        release_scopes(0)
        c.w('return %d;' % n[3])
        c.ind -= 1
        c.w('}')
        c.w('acc += %s;' % n[1])
    elif k == 'oe_brk':
        c.w('int %s = tv(%d, sel);' % (n[1], n[2]))
        c.w('if (!%s) {' % n[1])
        c.ind += 1
        release_scopes(loops[-1][1])
        c.w('break;')
        c.ind -= 1
        c.w('}')
        c.w('acc += %s;' % n[1])
    elif k == 'oe_cont':
        c.w('int %s = tv(%d, sel);' % (n[1], n[2]))
        c.w('if (!%s) {' % n[1])
        c.ind += 1
        release_scopes(next(l[1] for l in reversed(loops) if l[0] == 'loop'))
        c.w('continue;')
        c.ind -= 1
        c.w('}')
        c.w('acc += %s;' % n[1])
    elif k == 'oe_goto':
        c.w('int %s = tv(%d, sel);' % (n[1], n[2]))
        c.w('if (!%s) {' % n[1])
        c.ind += 1
        release_scopes(0)
        c.w('goto tail;')
        c.ind -= 1
        c.w('}')
        c.w('acc += %s;' % n[1])
    elif k == 'oe_blk':
        c.w('int %s = tv(%d, sel);' % (n[1], n[2]))
        c.w('if (!%s) {' % n[1])
        c.ind += 1
        scopes.append([])
        for x in n[3]:
            emit_ref(x, c, scopes, loops)
        close_scope()
        c.ind -= 1
        c.w('}')
        c.w('acc += %s;' % n[1])
    elif k == 'stmt':
        c.w(n[1])
    elif k == 'mut':
        c.w('%s = %s;' % (n[1], n[2]))
    elif k == 'stmtexpr':
        c.w('int %s;' % n[1])
        c.w('{')
        c.ind += 1
        scopes.append([])
        for x in n[2]:
            emit_ref(x, c, scopes, loops)
        close_scope()
        c.w('%s = %d;' % (n[1], n[3]))
        c.ind -= 1
        c.w('}')
        c.w('acc += %s;' % n[1])
    elif k == 'goto_tail':
        release_scopes(0)
        c.w('goto tail;')
    else:
        raise Exception('ref emit: ' + k)

def unit(name, body):
    global uid
    uid += 1
    c = Ctx()
    c.w('tt_begin();')
    c.w('int acc = 0;')
    emit_prism(('seq', body), c)
    c.w('goto tail;')
    c.w('tail:')
    c.w('return acc + acc_side;')
    prism = ['static int tor_%03d(int sel) {' % uid] + c.out + ['}']
    c = Ctx()
    c.w('tw_begin();')
    c.w('int acc = 0;')
    emit_ref(('seq', body), c, [[]], [])
    c.w('goto tail;')
    c.w('tail:')
    c.w('return acc + acc_side;')
    ref = ['static int ref_%03d(int sel) {' % uid] + c.out + ['}']
    units.append((uid, name, prism, ref))

D = 0
def fid():
    global D
    D += 1
    return D % 8

VN = [0]
def vname():
    VN[0] += 1
    return 'v%d' % VN[0]

# ------------------------------------------------------------------ corpus
unit('flat lifo', [
    ('defer', 1), ('defer', 2), ('defer', 3),
    ('oe_decl', 'a', 0, 40), ('ret', 'acc')])

unit('nested scopes unwind inner-first', [
    ('defer', 1),
    ('block', [('defer', 2), ('block', [('defer', 3), ('oe_decl', 'a', 1, 7)])]),
    ('defer', 4), ('ret', 'acc')])

unit('early return unwinds all', [
    ('defer', 1), ('block', [('defer', 2),
        ('if', 'sel == 1', [('ret', '100')], None)]),
    ('ret', 'acc')])

unit('orelse return action fires defers', [
    ('defer', 1), ('defer', 2), ('oe_ret', 'a', 2, -7), ('ret', 'acc')])

unit('loop defers fire per iteration', [
    ('defer', 1),
    ('for', 3, [('defer', 2), ('defer', 3), ('oe_decl', 'a', 3, 5)]),
    ('ret', 'acc')])

unit('break unwinds loop-body defers', [
    ('defer', 1),
    ('for', 3, [('defer', 2), ('if', 'i1 == sel', [('brk',)], None), ('defer', 3)]),
    ('ret', 'acc')])

unit('continue unwinds loop-body defers', [
    ('defer', 1),
    ('for', 3, [('defer', 2), ('if', 'i1 == sel', [('cont',)], None), ('defer', 3)]),
    ('ret', 'acc')])

unit('orelse break in loop', [
    ('defer', 1), ('for', 3, [('defer', 2), ('oe_brk', 'a', 4)]), ('ret', 'acc')])

unit('orelse continue in loop', [
    ('defer', 1), ('for', 3, [('defer', 2), ('oe_cont', 'a', 5)]), ('ret', 'acc')])

unit('while + nested for + mixed exits', [
    ('defer', 1),
    ('while', 3, [('defer', 2),
        ('for', 2, [('defer', 3), ('if', 'sel == 2', [('brk',)], None)]),
        ('if', 'sel == 1', [('ret', '55')], None)]),
    ('ret', 'acc')])

unit('do-while defers fire per iteration incl. continue', [
    ('defer', 1),
    ('do', 3, [('defer', 2), ('if', 'w2 == sel', [('cont',)], None), ('defer', 3)]),
    ('ret', 'acc')])

unit('goto to tail unwinds everything', [
    ('defer', 1), ('block', [('defer', 2),
        ('if', 'sel != 0', [('goto_tail',)], None), ('stmt', 'acc += 3;')]),
    ('ret', 'acc')])

unit('orelse goto action unwinds', [
    ('defer', 1), ('block', [('defer', 2), ('oe_goto', 'g', 3)]),
    ('ret', 'acc')])

unit('block-form defer bodies', [
    ('deferblk', 1), ('block', [('deferblk', 2)]),
    ('oe_decl', 'a', 6, 9), ('ret', 'acc')])

unit('capture-at-fire: defer sees final value', [
    ('stmt', 'int n = 1;'),
    ('deferv', 1, 'n'),
    ('mut', 'n', 'n + sel'),
    ('block', [('deferv', 2, 'n'), ('mut', 'n', '40')]),
    ('mut', 'n', '7'),
    ('ret', 'acc')])

unit('capture-at-fire across loop iterations', [
    ('stmt', 'int n = 0;'),
    ('for', 3, [('deferv', 1, 'n'), ('mut', 'n', 'n + i1 + 1')]),
    ('deferv', 2, 'n'),
    ('ret', 'acc')])

unit('switch braced cases with defers', [
    ('defer', 1),
    ('switch', [
        [('defer', 2), ('oe_decl', 'a', 0, 11)],
        [('defer', 3), ('defer', 4)],
        [('oe_decl', 'b', 3, 12)]]),
    ('ret', 'acc')])

unit('switch inside loop with orelse', [
    ('defer', 1),
    ('for', 2, [
        ('switch', [
            [('defer', 2)],
            [('oe_decl', 'a', 4, 13)],
            [('defer', 3), ('oe_chain', 'b', 0, 4, 14)]])]),
    ('ret', 'acc')])

unit('stmt-expr with inner-block defer', [
    ('defer', 1),
    ('stmtexpr', 's', [('block', [('defer', 2), ('oe_decl', 'a', 4, 6)]),
                       ('stmt', 'acc += 2;')], 21),
    ('ret', 'acc')])

unit('orelse block action with inner defer', [
    ('defer', 1),
    ('oe_blk', 'v', 3, [('defer', 2), ('stmt', 'acc += 5;')]),
    ('ret', 'acc')])

unit('orelse block action with break inside loop', [
    ('defer', 1),
    ('for', 3, [('defer', 2),
        ('oe_blk', 'v', 2, [('defer', 3), ('stmt', 'acc += 1;')])]),
    ('ret', 'acc')])

unit('orelse chain evaluation order', [
    ('defer', 1),
    ('oe_chain', 'a', 0, 1, 30),
    ('oe_chain3', 'b', 3, 3, 2, 31),
    ('ret', 'acc')])

unit('bare assign fallback', [
    ('defer', 1), ('stmt', 'int x = 0;'), ('oe_assign', 'x', 4, 12), ('ret', 'acc')])

unit('deep nesting mixed', [
    ('defer', 1),
    ('block', [('defer', 2),
        ('for', 2, [('defer', 3),
            ('block', [('defer', 4),
                ('if', 'i2 == 1 && sel == 1', [('ret', '77')], [('oe_decl', 'q', 5, 3)])])])]),
    ('defer', 5), ('ret', 'acc')])

# orelse INSIDE a defer block body: twin is hand-lowered (the walker treats
# defer bodies as opaque payloads, but here the payload itself uses orelse).
def unit_defer_orelse_body():
    global uid
    uid += 1
    prism = [
        'static int tor_%03d(int sel) {' % uid,
        '\ttt_begin();',
        '\tint acc = 0;',
        '\ttt_acquire(1);',
        '\tdefer { int q = tv(4, sel) orelse 5; acc_side += q; tt_release(1); }',
        '\tint a = tv(0, sel) orelse 8;',
        '\tacc += a;',
        '\tgoto tail;',
        'tail:',
        '\treturn acc + acc_side;',
        '}']
    ref = [
        'static int ref_%03d(int sel) {' % uid,
        '\ttw_begin();',
        '\tint acc = 0;',
        '\ttw_acquire(1);',
        '\tint a = tv(0, sel);',
        '\tif (!a) a = 8;',
        '\tacc += a;',
        '\t/* return value is evaluated BEFORE defers fire (SPEC defer #6) */',
        '\tint rv = acc + acc_side;',
        '\t{',
        '\t\tint q = tv(4, sel);',
        '\t\tif (!q) q = 5;',
        '\t\tacc_side += q;',
        '\t\ttw_release(1);',
        '\t}',
        '\tgoto tail;',
        'tail:',
        '\treturn rv;',
        '}']
    units.append((uid, 'orelse inside defer block body', prism, ref))
unit_defer_orelse_body()

# ------------------------------------------------- randomized structural storm
def rand_body(depth, in_loop, ids, budget, in_se):
    body = []
    for _ in range(rng.randint(2, 4)):
        if budget[0] <= 0:
            break
        budget[0] -= 1
        r = rng.random()
        if r < 0.24 and ids[0] < 60:
            ids[0] += 1
            kind = rng.random()
            if kind < 0.55:
                body.append(('defer', ids[0]))
            elif kind < 0.8:
                body.append(('deferblk', ids[0]))
            else:
                body.append(('deferv', ids[0], 'acc'))
        elif r < 0.40:
            v = vname()
            kind = rng.choice(['oe_decl', 'oe_chain', 'oe_chain3', 'oe_blk'])
            if kind == 'oe_decl':
                body.append(('oe_decl', v, fid(), rng.randint(1, 20)))
            elif kind == 'oe_chain':
                body.append(('oe_chain', v, fid(), fid(), rng.randint(1, 20)))
            elif kind == 'oe_chain3':
                body.append(('oe_chain3', v, fid(), fid(), fid(), rng.randint(1, 20)))
            else:
                inner = [('stmt', 'acc += %d;' % rng.randint(1, 5))]
                if rng.random() < 0.5 and ids[0] < 60:
                    ids[0] += 1
                    inner.insert(0, ('defer', ids[0]))
                body.append(('oe_blk', v, fid(), inner))
        elif r < 0.52 and depth < 4:
            body.append(('block', rand_body(depth + 1, in_loop, ids, budget, in_se)))
        elif r < 0.62 and depth < 3 and not in_se:
            loop = rng.choice(['for', 'while', 'do'])
            body.append((loop, rng.randint(1, 3),
                         rand_body(depth + 1, True, ids, budget, in_se)))
        elif r < 0.68 and depth < 2 and not in_se and not in_loop:
            body.append(('switch', [rand_body(depth + 1, False, ids, budget, in_se)
                                    for _ in range(rng.randint(2, 3))]))
        elif r < 0.76 and in_loop:
            body.append(('if', 'sel == %d' % rng.randint(0, SELS - 1),
                         [rng.choice([('brk',), ('cont',)])], None))
        elif r < 0.86:
            body.append(('if', 'sel %s %d' % (rng.choice(['==', '!=', '>']),
                                              rng.randint(0, SELS - 1)),
                         rand_body(depth + 1, in_loop, ids, budget, in_se),
                         rand_body(depth + 1, in_loop, ids, budget, in_se)
                         if rng.random() < 0.5 else None))
        elif r < 0.90 and not in_se:
            body.append(('oe_ret', vname(), fid(), rng.randint(-9, -1)))
        elif r < 0.93 and in_loop:
            body.append(rng.choice([('oe_brk', vname(), fid()),
                                    ('oe_cont', vname(), fid())]))
        elif r < 0.96 and depth < 2 and not in_se and not in_loop:
            body.append(('stmtexpr', vname(),
                         [('block', rand_body(depth + 1, False, ids, budget, True)),
                          ('stmt', 'acc += 1;')], rng.randint(1, 9)))
        else:
            body.append(('stmt', 'acc += %d;' % rng.randint(1, 9)))
    return body

for i in range(120):
    ids = [10]
    budget = [18]
    unit('storm %d' % i, rand_body(0, False, ids, budget, False) + [('ret', 'acc')])

# ----------------------------------------------------- bounded-exhaustive tier
# COMPLETE product: wrapper x outer-defer-count x inner-defer-count x exit
# kind.  Unlike the seeded 'storm' units above (samples), this tier is
# exhaustive over its grammar.  Justification for the bound (see
# .github/defer_model.h and test.machine.c): defer_walk's transition coverage
# saturates at scope depth 3 — every (exit-kind x scope-symbol x
# has-defers) machine transition occurs within this product — so running
# each cell end-to-end (parse -> emit -> execute vs. the independent
# reference lowering, with LIFO/trace/return/eval-parity assertions) extends
# the abstract-machine proof to the full concrete pipeline.
EXH_WRAPPERS = ['none', 'block', 'for', 'while', 'do', 'switch']
EXH_EXITS = ['end', 'ret', 'brk', 'cont', 'oe_decl', 'oe_ret', 'oe_brk', 'oe_cont']
for exh_wrap in EXH_WRAPPERS:
    for exh_nout in (0, 1, 2):
        for exh_nin in (0, 1, 2):
            for exh_ex in EXH_EXITS:
                exh_loopy = exh_wrap in ('for', 'while', 'do')
                exh_brky = exh_loopy or exh_wrap == 'switch'
                if exh_ex in ('brk', 'oe_brk') and not exh_brky:
                    continue
                if exh_ex in ('cont', 'oe_cont') and not exh_loopy:
                    continue
                inner = [('defer', 10 + j) for j in range(exh_nin)]
                if exh_ex == 'ret':
                    inner.append(('if', 'sel == 1', [('ret', '77')], None))
                elif exh_ex == 'brk':
                    inner.append(('if', 'sel == 1', [('brk',)], None))
                elif exh_ex == 'cont':
                    inner.append(('if', 'sel == 1', [('cont',)], None))
                elif exh_ex == 'oe_decl':
                    inner.append(('oe_decl', vname(), fid(), 9))
                elif exh_ex == 'oe_ret':
                    inner.append(('oe_ret', vname(), fid(), -3))
                elif exh_ex == 'oe_brk':
                    inner.append(('oe_brk', vname(), fid()))
                elif exh_ex == 'oe_cont':
                    inner.append(('oe_cont', vname(), fid()))
                if exh_wrap == 'none':
                    mid = inner
                elif exh_wrap == 'block':
                    mid = [('block', inner)]
                elif exh_wrap == 'for':
                    mid = [('for', 2, inner)]
                elif exh_wrap == 'while':
                    mid = [('while', 2, inner)]
                elif exh_wrap == 'do':
                    mid = [('do', 2, inner)]
                else:
                    mid = [('switch', [inner, [('brk',)]])]
                exh_body = [('defer', j + 1) for j in range(exh_nout)] + mid + \
                           [('ret', 'acc')]
                unit('exh %s o%d i%d %s' % (exh_wrap, exh_nout, exh_nin, exh_ex),
                     exh_body)

# ------------------------------------------------------------ misuse corpus
# Every entry must be REJECTED pre-emit (status != PRISM_OK). Focused on
# defer x orelse x context INTERSECTIONS; flat per-feature rejects live in
# test.safe.c's phase-1 corpus.
MISUSE = [
 ("defer at file scope", "void g(void);\ndefer g();\n"),
 ("defer in for-init", "void g(void);\nvoid f(void){ for (defer g(); 0;) {} }\n"),
 ("defer in ctrl paren", "int g(void);\nvoid f(void){ if (defer g()) {} }\n"),
 ("defer top-level of stmt-expr",
  "void g(void);\nvoid f(void){ int x = ({ defer g(); 1; }); (void)x; }\n"),
 ("defer block last in stmt-expr",
  "void g(void);\nvoid f(void){ int x = ({ int r = 1; { defer g(); } }); (void)x; }\n"),
 ("return inside defer body", "void f(void){ defer { return; } }\n"),
 ("goto inside defer body", "void f(void){ defer { goto out; } out:; }\n"),
 ("break inside defer body in loop",
  "void f(void){ for (int i=0;i<2;i++) { defer { break; } } }\n"),
 ("continue inside defer body in loop",
  "void f(void){ for (int i=0;i<2;i++) { defer { continue; } } }\n"),
 ("nested defer", "void g(void);\nvoid f(void){ defer { defer g(); } }\n"),
 ("label inside defer body", "void g(void);\nvoid f(void){ defer { L: g(); } }\n"),
 ("defer with vfork in function",
  "int vfork(void);\nvoid g(void);\nvoid f(void){ defer g(); if (vfork()) return; }\n"),
 ("computed goto with defer",
  "void g(void);\nvoid f(void){ void *p = &&L; defer g(); goto *p; L:; }\n"),
 ("forward goto skips defer",
  "void g(void);\nvoid f(void){ goto L; defer g(); L:; }\n"),
 ("backward goto loops over defer",
  "void g(void);\nvoid f(int n){ L:; defer g(); if (n--) goto L; }\n"),
 ("case label into scope with defer (Duff)",
  "void g(void);\nvoid f(int n){ switch (n) { case 0: { defer g(); case 1: ; } break; } }\n"),
 ("orelse at file scope", "int g(void);\nint x = g() orelse 1;\n"),
 ("orelse static init",
  "int g(void);\nvoid f(void){ static int x = g() orelse 1; (void)x; }\n"),
 ("orelse in plain enum", "int g(void);\nvoid f(void){ enum { A = g() orelse 1 }; }\n"),
 ("orelse in fixed enum",
  "int g(void);\nenum E : unsigned { A = g() orelse 1 };\n"),
 ("orelse in struct body",
  "int g(void);\nstruct S { int a[2 orelse 3]; };\n"),
 ("orelse in ternary",
  "int g(void);\nvoid f(void){ int x = g() ? g() orelse 1 : 2; (void)x; }\n"),
 ("orelse in non-wrap parens",
  "int g(void);\nvoid f(void){ int x = 1 + (g() orelse 2); (void)x; }\n"),
 ("orelse in for-init",
  "int g(void);\nvoid f(void){ for (int i = g() orelse 1; i < 2; i++) {} }\n"),
 ("orelse stmt action in ctrl paren",
  "int g(void);\nvoid f(void){ if (g() orelse return) {} }\n"),
 ("orelse in attr args",
  "int g(void);\nvoid f(void){ __attribute__((aligned(4 orelse 8))) int x; (void)x; }\n"),
 ("orelse missing action", "int g(void);\nvoid f(void){ int x; x = g() orelse; }\n"),
 ("orelse missing action comma",
  "int g(void);\nvoid f(void){ int x; x = g() orelse, 2; }\n"),
 ("orelse chain tail missing action",
  "int g(void);\nint h(void);\nvoid f(void){ int x; x = g() orelse h() orelse; }\n"),
 ("decl orelse chain tail missing",
  "int g(void);\nvoid f(void){ int x = g() orelse 1 orelse; (void)x; }\n"),
 ("orelse on struct value",
  "struct S { int a; };\nstruct S mk(void);\nvoid f(void){ struct S s = mk() orelse return; (void)s; }\n"),
 ("orelse const struct value",
  "struct S { int a; };\nstruct S mk(void);\nvoid f(void){ const struct S s = mk() orelse return; (void)s; }\n"),
 ("orelse array decl",
  "int g(void);\nvoid f(void){ int a[3] = {0} orelse return; (void)a; }\n"),
 ("const VLA orelse fallback",
  "int g(void);\nvoid f(int n){ const int a[n] = g() orelse 1; (void)a; }\n"),
 ("bracket orelse with return action",
  "int g(void);\nvoid f(void){ int a[g() orelse return]; (void)a; }\n"),
 ("bracket orelse block action",
  "int g(void);\nvoid f(void){ int a[g() orelse { }]; (void)a; }\n"),
 ("bracket orelse at file scope",
  "int n;\nint arr[n orelse 4];\n"),
 ("bracket orelse in proto param",
  "void f(int n, int arr[n orelse 2]);\n"),
 ("bracket orelse in ctrl decl",
  "int g(void);\nvoid f(void){ for (int a[g() orelse 2]; 0;) {} }\n"),
 ("chained bracket orelse SE",
  "int g(void);\nvoid f(void){ int a[g() orelse g() orelse 2]; (void)a; }\n"),
 ("bare orelse without target",
  "int g(void);\nvoid f(void){ g() orelse 5; }\n"),
 ("bare orelse cast LHS",
  "int g(void);\nvoid f(void){ int x = 0; (int)x = g() orelse 5; }\n"),
 ("orelse GNU stmt-expr fallback in decl",
  "int g(void);\nvoid f(void){ int x = g() orelse ({ 1; }); (void)x; }\n"),
 ("braceless defer body decl with orelse init",
  "int g(void);\nvoid f(void){ defer int t = g() orelse 1; }\n"),
 ("defer inside typeof",
  "void g(void);\nvoid f(void){ typeof(({ { defer g(); } 0; })) v = 0; (void)v; }\n"),
 ("orelse inside _Alignas",
  "int g(void);\nvoid f(void){ _Alignas(4 orelse 8) int x; (void)x; }\n"),
 ("register VLA zero-init",
  "void f(int n){ register int a[n]; (void)a; }\n"),
 ("register union zero-init",
  "union U { int a; char b[8]; };\nvoid f(void){ register union U u; (void)u; }\n"),
 ("nested fn with orelse body",
  "int main(void){ int o = 5; int nested(void){ int x = o orelse return 0; return x; } return nested(); }\n"),
 ("goto into stmt-expr",
  "void f(void){ goto in; int x = ({ in: 1; }); (void)x; }\n"),
 ("case bypasses zero-init decl",
  "void f(int n){ switch (n) { case 0: { int z; case 1: z = 1; (void)z; } } }\n"),
 ("switch braced trapdoor decl",
  "void f(int n){ switch (n) { int z; case 0: z = 1; break; } }\n"),
 ("defer between switch cases",
  "void g(void);\nvoid f(int n){ switch (n) { case 0: defer g(); case 1: break; } }\n"),
 ("anon struct multidecl bracket orelse split",
  "int g(void);\nvoid f(void){ struct { int x; } a, b[g() orelse 2]; (void)a; (void)b; }\n"),
 ("multidecl typeof-VM split via orelse",
  "int g(void);\nvoid f(int n){ typeof(int[n]) a, b = {0} orelse return; (void)a; (void)b; }\n"),
]

# ------------------------------------------------------------------- output
print('// GENERATED FILE — do not edit by hand.')
print('// Produced by gen_torture.py (deterministic seed). Regenerate:')
print('//   python3 .github/gen_torture.py > .github/test.torture.c')
print('//')
print('// Semantic torture tier for defer + orelse. Valid units exist twice:')
print('//   tor_N — Prism source (defer/orelse), transpiled by the pipeline')
print('//   ref_N — plain-C twin, cleanup placed by an INDEPENDENT lowering')
print('// Per (unit, sel): LIFO stack discipline, exactly-once firing, zero')
print('// leaks, identical fire traces (ids + captured values), identical')
print('// returns, identical value-expression evaluation counts.')
print('// Misuse units must be rejected pre-emit via prism_transpile_source.')
print()
print('#define TOR_MAX 64')
print('#define TOR_LOG 1024')
print('static _Thread_local int tt_acq[TOR_MAX], tt_rel[TOR_MAX];')
print('static _Thread_local int tt_stack[TOR_MAX], tt_top, tt_viol;')
print('static _Thread_local long tt_log[TOR_LOG]; static _Thread_local int tt_logn;')
print('static _Thread_local int tw_acqc[TOR_MAX], tw_relc[TOR_MAX];')
print('static _Thread_local long tw_log[TOR_LOG]; static _Thread_local int tw_logn;')
print('static _Thread_local int tt_evals[8], tw_evals[8], tor_ref_mode;')
print('static _Thread_local int tt_touches, acc_side;')
print()
print('static void tt_begin(void) {')
print('\tmemset(tt_acq, 0, sizeof(tt_acq)); memset(tt_rel, 0, sizeof(tt_rel));')
print('\tmemset(tt_evals, 0, sizeof(tt_evals));')
print('\ttt_top = 0; tt_viol = 0; tt_logn = 0; tor_ref_mode = 0; acc_side = 0;')
print('}')
print('static void tw_begin(void) {')
print('\tmemset(tw_acqc, 0, sizeof(tw_acqc)); memset(tw_relc, 0, sizeof(tw_relc));')
print('\tmemset(tw_evals, 0, sizeof(tw_evals));')
print('\ttw_logn = 0; tor_ref_mode = 1; acc_side = 0;')
print('}')
print('static void tt_acquire(int id) {')
print('\ttt_acq[id]++;')
print('\tif (tt_top < TOR_MAX) tt_stack[tt_top++] = id;')
print('}')
print('static void tt_release(int id) {')
print('\ttt_rel[id]++;')
print('\tif (tt_top <= 0 || tt_stack[tt_top - 1] != id) tt_viol++;')
print('\telse tt_top--;')
print('\tif (tt_logn < TOR_LOG) tt_log[tt_logn++] = id;')
print('}')
print('static void tt_release_val(int id, int v) {')
print('\ttt_rel[id]++;')
print('\tif (tt_top <= 0 || tt_stack[tt_top - 1] != id) tt_viol++;')
print('\telse tt_top--;')
print('\tif (tt_logn < TOR_LOG) tt_log[tt_logn++] = (long)id * 4096 + v;')
print('}')
print('static void tw_acquire(int id) { tw_acqc[id]++; }')
print('static void tw_release(int id) {')
print('\ttw_relc[id]++;')
print('\tif (tw_logn < TOR_LOG) tw_log[tw_logn++] = id;')
print('}')
print('static void tw_release_val(int id, int v) {')
print('\ttw_relc[id]++;')
print('\tif (tw_logn < TOR_LOG) tw_log[tw_logn++] = (long)id * 4096 + v;')
print('}')
print('static void tt_touch(void) { tt_touches++; }')
print('/* Deterministic value oracle; counts evaluations for double-eval parity. */')
print('static int tv(int f, int sel) {')
print('\tif (tor_ref_mode) tw_evals[f & 7]++;')
print('\telse tt_evals[f & 7]++;')
print('\tswitch (f & 7) {')
print('\tcase 0: return sel;')
print('\tcase 1: return sel - 1;')
print('\tcase 2: return sel - 2;')
print('\tcase 3: return 0;')
print('\tcase 4: return 7;')
print('\tcase 5: return sel & 1;')
print('\tcase 6: return 3 - sel;')
print('\tdefault: return sel - 3;')
print('\t}')
print('}')
print()
for _, name, prism, ref in units:
    print('/* torture: %s */' % name)
    print('\n'.join(prism))
    print('\n'.join(ref))
    print()
print('typedef int (*tor_fn)(int);')
print('static const struct { const char *name; tor_fn tor, ref; } tor_units[] = {')
for n, name, _, _ in units:
    print('\t{"%s", tor_%03d, ref_%03d},' % (name, n, n))
print('};')
print()
print('static const struct { const char *name, *src; } tor_misuse[] = {')
for name, src in MISUSE:
    esc = src.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n')
    print('\t{"%s",\n\t "%s"},' % (name, esc))
print('};')
print()
print('static void run_torture_tests(void) {')
print('\tprintf("\\n=== TORTURE: defer/orelse semantic invariants ===\\n");')
print('\tint bad = 0;')
print('\tfor (size_t u = 0; u < sizeof(tor_units) / sizeof(tor_units[0]); u++) {')
print('\t\tfor (int sel = 0; sel < %d; sel++) {' % SELS)
print('\t\t\tint rt = tor_units[u].tor(sel);')
print('\t\t\tint tviol = tt_viol, ttop = tt_top;')
print('\t\t\tint rr = tor_units[u].ref(sel);')
print('\t\t\tbool once = true, trace = tt_logn == tw_logn, evals = true;')
print('\t\t\tfor (int i = 0; i < TOR_MAX; i++) {')
print('\t\t\t\tif (tt_acq[i] != tt_rel[i]) once = false;')
print('\t\t\t\tif (tt_acq[i] != tw_acqc[i] || tt_rel[i] != tw_relc[i]) trace = false;')
print('\t\t\t}')
print('\t\t\tfor (int i = 0; i < tt_logn && i < tw_logn; i++)')
print('\t\t\t\tif (tt_log[i] != tw_log[i]) trace = false;')
print('\t\t\tfor (int i = 0; i < 8; i++)')
print('\t\t\t\tif (tt_evals[i] != tw_evals[i]) evals = false;')
print('\t\t\tbool ok = tviol == 0 && ttop == 0 && once && trace && evals && rt == rr;')
print('\t\t\ttotal++;')
print('\t\t\tif (ok) passed++;')
print('\t\t\telse {')
print('\t\t\t\tbad++;')
print('\t\t\t\tprintf("[FAIL] torture \\"%s\\" sel=%d: viol=%d live=%d once=%d '
      'trace=%d evals=%d ret=%d/%d\\n",')
print('\t\t\t\t       tor_units[u].name, sel, tviol, ttop, (int)once,')
print('\t\t\t\t       (int)trace, (int)evals, rt, rr);')
print('\t\t\t}')
print('\t\t}')
print('\t}')
print('\tif (!bad)')
print('\t\tprintf("[PASS] %d torture unit-runs: LIFO + exactly-once + trace + "')
print('\t\t       "eval-parity + return equality\\n",')
print('\t\t       (int)(sizeof(tor_units) / sizeof(tor_units[0])) * %d);' % SELS)
print('\tint rbad = 0;')
print('\tfor (size_t m = 0; m < sizeof(tor_misuse) / sizeof(tor_misuse[0]); m++) {')
print('\t\tPrismResult r = prism_transpile_source(tor_misuse[m].src, "tor_misuse.c",')
print('\t\t\t\t\t\t       prism_defaults());')
print('\t\ttotal++;')
print('\t\tif (r.status != PRISM_OK) passed++;')
print('\t\telse {')
print('\t\t\trbad++;')
print('\t\t\tprintf("[FAIL] torture misuse accepted: %s\\n", tor_misuse[m].name);')
print('\t\t}')
print('\t\tprism_free(&r);')
print('\t}')
print('\tif (!rbad)')
print('\t\tprintf("[PASS] %d misuse units all rejected pre-emit\\n",')
print('\t\t       (int)(sizeof(tor_misuse) / sizeof(tor_misuse[0])));')
print('}')
