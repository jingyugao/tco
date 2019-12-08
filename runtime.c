#include "runtime.h"
#include "os.h"
#include "proc.h"
#include "time2.h"
#include "timer.h"
#include <pthread.h>
#define _StackReserve (1 << 20)
#define _StackMin (2 << 10)

#define ALIGN(p, alignbytes)                                                   \
	((void *)(((unsigned long)(p) + (alignbytes)-1) & ~((alignbytes)-1)))

m *m0;
g *g0;
g *allgs[1024];
_Atomic int main_started;
static pthread_key_t VirFSReg = 0; // pointer to m.tls
void settls(tls *ptr)
{
	int ret = pthread_setspecific(VirFSReg, ptr);
	if (ret != 0) {
		printf("pthread_setspecific error:%d\n", ret);
		abort();
	}
}

tls *gettls()
{
	return (tls *)pthread_getspecific(VirFSReg);
}

p *getP()
{
	return getg()->m->p;
};

// ctx must be within g struct
void gogo(Context *ctx)
{
	g *gp = container_of(ctx, g, ctx);
	// g *gp = (g *)((void *)ctx - (void *)(&((g *)NULL)->ctx));
	tls *tls = gettls();
	tls->ptr[0] = (uintptr)gp;
	GetContext(ctx);
}

void SwitchTo(g *from, g *to)
{
	to->m = from->m;
	printf("switch %d to %d\n", from->id, to->id);
	int ret = SaveContext(&from->ctx);
	if (ret == 0) {
		gogo(&to->ctx);
	}
}

int allocGID()
{
	static int gid = 1;
	gid++;
	return gid;
}

void allgadd(g *gp)
{
	for (int i = 0; i < 1024; i++) {
		if (allgs[i] == NULL) {
			allgs[i] = gp;
			return;
		}
	}
	assert(0);
	return;
}

g *malg()
{
	int stackSize = _StackReserve;
	uintptr stackTop;

	int ret = posix_memalign((void **)&stackTop, stackSize, stackSize);
	if (ret != 0) {
		return NULL;
	}
	uintptr stackBase = stackTop + stackSize;
	g *c = newT(g);
	c->stack.lo = stackTop;
	// align
	stackBase = (uintptr)ALIGN(stackBase, 16);
	stackBase = stackBase - 8;
	*(uintptr *)stackBase = (uintptr)goexit;

	c->stack.hi = stackBase;
	c->ctx.reg.rsp = stackBase;

	return c;
}

void runqput(p *p, g *g)
{
	if (g->id == 0) {
		abort();
	}
	pthread_mutex_lock(&p->mu);
	int h = p->runqhead;
	int t = p->runqtail;

	int size = (sizeof(p->runq) / sizeof(p->runq[0]));
	if (t - h < size) {
		p->runq[t % (size)] = g;
		p->runqtail = t + 1;
	}
	pthread_mutex_unlock(&p->mu);

	// put to global runq
};

g *runqget(p *p)
{
	pthread_mutex_lock(&p->mu);
	if (p->runqhead == p->runqtail) {
		pthread_mutex_unlock(&p->mu);
		return NULL;
	}
	int size = (sizeof(p->runq) / sizeof(p->runq[0]));
	g *c = p->runq[p->runqhead % size];
	p->runqhead++;
	pthread_mutex_unlock(&p->mu);
	return c;
}

void casgstatus(g *gp, uint32 oldval, uint32 newval)
{
	gp->atomicstatus = newval;
}

void aligncall(Func fn){
	
}

void mcall(void (*f)(g *))
{
	printf("mcall\n");
	g*gp=getg();
	g*g0=gp->m->g0;
	printf("getg%d\n",gp->id);
	g0->ctx.reg.rdi = (uintptr)gp;
	g0->ctx.reg.pc_addr = (uintptr)f;
	g0->m->tls.ptr[0] = (uintptr)g0;
	SwitchTo(gp,g0);
}

void goexit0(g *gp)
{
	printf("goexit0\n");
	casgstatus(gp, gp->atomicstatus, _Gdead);
	schedule();
}

void goexit1()
{
	mcall(goexit0);
}

void goexit()
{
	printf("g %d exit\n", getg()->id);
	goexit1();
}

void Gosched()
{
	g *curg = getg();
	p *p = getP();
	runqput(p, curg);
	casgstatus(curg, curg->atomicstatus, _Grunnable);
	SwitchTo(curg, curg->m->g0);
	return;
};

g *getg()
{
	return ((g *)gettls()->ptr[0]);
}

void systemstack(Func fn)
{
	printf("systemstack\n");
	g0->fn = fn;
	int ret = SaveContext(&g0->ctx);
	printf("ret:%d\n", ret);
	if (ret == 0) {
		((void (*)(uintptr))(g0->fn.f))(g0->fn.arg);
		GetContext(&g0->ctx);
	}
}

g *newproc1(Func fn)
{
	g *newg = malg();
	allgadd(newg);
	int gid = allocGID();
	newg->id = gid;
	newg->fn = fn;
	newg->ctx.reg.rdi = (uintptr)newg->fn.arg;
	newg->ctx.reg.pc_addr = (uintptr)newg->fn.f;
	casgstatus(newg, newg->atomicstatus, _Grunnable);
	runqput(getg()->m->p, newg);

	if (atomic_load(&sched.npidle) != 0 &&
	    atomic_load(&main_started) == 1) {
		wakep();
	}
	return newg;
}

void newproc(uintptr f, uintptr arg)
{
	Func fn;
	fn.f = f;
	fn.arg = arg;
	newproc1(fn);
	return;
}

void schedinit()
{
	printf("schedinit\n");
	g *_g_ = getg();
	void *xx;
	for (int i = 0; i < MAXPORC; i++) {
		p *_p_ = newT(p);
		// memset(allp[i], 0, sizeof(p));
		_p_->id = i;
		_p_->link = sched.pidle;
		if (i != 0) {
			atomic_fetch_add(&sched.npidle, 1);
			sched.pidle = _p_;
		}
		allp[i] = _p_;
	}

	_g_->m->p = allp[0];
	allp[0]->m = _g_->m;
}

int main_main();

g *runqsteal(p *_p_, p *p2, bool stealRunNextG)
{
	if (p2 == NULL || p2 == _p_) {
		return NULL;
	}
	g *nextg = runqget(p2);
	if (nextg != NULL) {
		return nextg;
	}
	return NULL;
}

void check_timers(p *pp, int64 ns);

g *findRunnable()
{
	printf("findrunnable\n");
	p *_p_ = getg()->m->p;
	check_timers(_p_, 0);
	g *nextg = runqget(_p_);
	if (nextg != NULL) {
		return nextg;
	}
	for (int i = 0; i < MAXPORC; i++) {
		nextg = runqsteal(_p_, allp[i], true);
		if (nextg != NULL) {
			printf("%d steal %d from %d\n", _p_->id, nextg->id,
			       allp[i]->id);
			break;
		}
	}
	return nextg;
}
void call_fn(Func fn)
{
	((void (*)(uintptr))(fn.f))(fn.arg);
}
void check_timers(p *pp, int64 ns)
{
	pthread_mutex_lock(&pp->timerslock);
	ns = nanotime();
	while (1) {
		if (vector_count(&pp->timers) == 0) {
			break;
		}
		timer *t0 = vector_get(&pp->timers, 0);
		if (t0->when > ns) {
			break;
		}
		call_fn(t0->fn);
		pop_timers(&pp->timers);
	}

	pthread_mutex_unlock(&pp->timerslock);
}

// must on g0
void schedule()
{
	printf("schedule %d\n", getg()->m->p->id);
	while (1) {
		g *nextg = findRunnable();
		if (nextg == NULL) {
			usleep(200 * 1000);
			printf("p%d no co to run:\n", getg()->m->p->id);
			continue;
		}
		SwitchTo(getg(), nextg);
	}
}

void wakeg(g *gp)
{
	p *_p_ = getg()->m->p;
	runqput(_p_, gp);
}

void timeSleep(int64 ns)
{
	if (ns <= 0) {
		return;
	}

	g *gp = getg();
	gp->when = nanotime() + ns;
	casgstatus(gp, gp->atomicstatus, _Gwaiting);
	timer *t = newT(timer);
	t->when = nanotime() + ns;
	t->fn.arg = (uintptr)gp;
	t->fn.f = (uintptr)wakeg;
	p *_p_ = gp->m->p;
	push_timers(&_p_->timers, t);
	SwitchTo(gp, gp->m->g0);
}

void mstart1()
{
	g *_g_ = getg();
	Func fn = _g_->m->mstartfn;
	if (fn.f != 0) {
		((void (*)(uintptr))(fn.f))(fn.arg);
	}

	schedule();
}

void mstart()
{
	mstart1();
	// never return
	abort();
	// pthread_exit(0);
}

int main();
// really main
int rt0_go()
{
	usleep(1);
	printf("asm main\n");
	memset(allgs, 0, 1024 * sizeof(uintptr));
	int ret = pthread_key_create(&VirFSReg, NULL);
	if (ret != 0) {
		printf("pthread_key_create error:%d\n", ret);
		abort();
	}
	// init g0
	Func fg0;
	fg0.f = (uintptr)schedule;
	g0 = malg();
	m *m0 = newT(m);
	settls(&m0->tls);
	m0->tls.ptr[0] = (uintptr)g0;
	g0->m = m0;
	m0->g0 = g0;
	m0->curg = g0;
	allgs[0] = g0;
	g0->id = 0;
	g0->fn = fg0;
	g0->ctx.reg.pc_addr = (long)g0->fn.f;

	schedinit();
	newproc((uintptr)main, 0);
	newm((uintptr)sysmon, 0);
	// sleep(1000);
	mstart();

	abort();
	return 0;
}

int main()
{
	if (!g0) {
		printf("main must called after rt0_go\n");
		abort();
	}
	atomic_store(&main_started, 1);
	main_main();
	exit(0);
	return 0;
}

void gopark(bool (*f)(g *, void *), void *lock, int reason)
{
}