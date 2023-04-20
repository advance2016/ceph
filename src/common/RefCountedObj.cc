// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
//
#include "include/ceph_assert.h"

#include "common/RefCountedObj.h"
#include "common/ceph_context.h"
#include "common/dout.h"
#include "common/valgrind.h"

namespace TOPNSPC::common {
RefCountedObject::~RefCountedObject()
{
  ceph_assert(nref == 0);
}

/*将该对象的引用计数减一，并在引用计数变为 0 时释放该对象

put() 函数会将该对象的引用计数减一，并将减一后的引用计数值赋给变量 v。如果 
CephContext 对象的指针不为空，则输出日志记录该对象的引用计数变化。
如果减一后的引用计数变为 0，则通过 ANNOTATE_HAPPENS_AFTER() 和 
ANNOTATE_HAPPENS_BEFORE_FORGET_ALL() 函数标记该对象的生命周期结束，然后释放该对象的内存空间

ANNOTATE_HAPPENS_AFTER() 和 ANNOTATE_HAPPENS_BEFORE_FORGET_ALL() 函数是 
Google 开源工具 Annotate的一部分，用于帮助分析内存中的对象生命周期。它们的
作用是标记对象的生命周期结束，以便在后续的内存泄漏检测中排除该对象。
*/
void RefCountedObject::put() const {
  CephContext *local_cct = cct;
  auto v = --nref;
  if (local_cct) {
    lsubdout(local_cct, refs, 1) << "RefCountedObject::put " << this << " "
		   << (v + 1) << " -> " << v
		   << dendl;
  }
  if (v == 0) {
    ANNOTATE_HAPPENS_AFTER(&nref);
    ANNOTATE_HAPPENS_BEFORE_FORGET_ALL(&nref);
    delete this;
  } else {
    /*
    如果减一后的引用计数不为 0，          则通过ANNOTATE_HAPPENS_BEFORE()函数标记该对象
    的引用计数发生了变化，但不进行释放操作。
    */
    ANNOTATE_HAPPENS_BEFORE(&nref);
  }
}

void RefCountedObject::_get() const {
  auto v = ++nref;
  ceph_assert(v > 1); /* it should never happen that _get() sees nref == 0 */
  if (cct) {
    lsubdout(cct, refs, 1) << "RefCountedObject::get " << this << " "
	     << (v - 1) << " -> " << v << dendl;
  }
}

}
