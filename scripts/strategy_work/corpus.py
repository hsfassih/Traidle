import json,glob,os
def as_list(x):
    if x is None: return []
    return x if isinstance(x,list) else [x]
def extract(d):
    S=[];E=[]
    def add_strat(s):
        s=dict(s)
        for k in ('chart_examples','pass_3_chart_examples'):
            if k in s:
                for e in as_list(s[k]): E.append(e)
        ex=s.get('examples')
        if isinstance(ex,list):
            for e in ex:
                if isinstance(e,dict): E.append(e)
        S.append(s)
    if isinstance(d,list):
        for x in d:
            if 'example_id' in x: E.append(x)
            elif 'strategy_id' in x: add_strat(x)
            else: print('UNKNOWN item',list(x)[:5])
    else:
        if 'strategy_id' in d: add_strat(d)
        for k in ('strategy_definition','strategies','strategy','pass_2_strategies'):
            for s in as_list(d.get(k)): add_strat(s)
        for k in ('chart_examples','examples','pass_3_examples'):
            if k in d and 'strategy_id' not in d:
                for e in as_list(d.get(k)): E.append(e)
    return S,E
def load_all(folder='strategies'):
    out={}
    for p in sorted(glob.glob(os.path.join(folder,'*.json'))):
        d=json.load(open(p,encoding='utf-8'))
        out[os.path.basename(p)]=(d,)+extract(d)
    return out

