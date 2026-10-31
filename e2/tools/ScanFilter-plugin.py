# -*- coding: utf-8 -*-
# ScanFilter v4 -- 扫描时自动屏蔽非电视服务（enigma2 进程内，无外部进程）。
#
# 三处挂钩：
#  1) scanStatusChanged 检测扫描完成：
#     saveServicelist()（把内存库强制落盘）→ 解析 lamedb5 → 对所有非电视
#     服务（类型 != 1：2=广播电台、12=数据业务 ad*/UI-*；或名字 ad 开头）
#     removeService → 再次 saveServicelist 回存。
#  2) ok()：E2 在关闭扫描结果页时构建 userbouquet.LastScanned.tv（来源是
#     屏幕的已发现列表，会包含非电视条目）。补丁在其后重写该 bouquet
#     文件并 reloadBouquets()。
#  3) newService：探测 getLastServiceRef（部分固件返回空，探测到有效引用
#     时顺手拦掉结果列表里的非电视条目）。
import glob
import time

LAMEDB5 = "/etc/enigma2/lamedb5"
LOG = "/tmp/scanfilter.log"

_timers = []


def _log(msg):
    try:
        with open(LOG, "a") as f:
            f.write(time.strftime("[%Y-%m-%d %H:%M:%S] ") + msg + "\n")
    except Exception:
        pass


def _unwanted_from_file():
    """解析 lamedb5，返回非电视服务的引用字符串列表。"""
    refs = []
    try:
        with open(LAMEDB5) as f:
            for line in f:
                if not line.startswith("s:"):
                    continue
                head = line.split(",")[0]
                tok = head.split(":")
                # s:sid:ns:tsid:onid:type:number:source
                if len(tok) < 6:
                    continue
                name = line.split('"')[1] if '"' in line else ""
                try:
                    stype = int(tok[5])
                    if stype == 1 and not name.lower().startswith("ad"):
                        continue
                    ref = "1:0:%x:%04x:%04x:%04x:%08x:0:0:0" % (
                        stype, int(tok[1], 16), int(tok[3], 16),
                        int(tok[4], 16), int(tok[2], 16))
                except ValueError:
                    continue
                refs.append(ref)
    except Exception as e:
        _log("parse %s error: %r" % (LAMEDB5, e))
    return refs


def _db_cleanup(tag):
    try:
        from enigma import eDVBDB, eServiceReference
        db = eDVBDB.getInstance()
        try:
            db.saveServicelist()   # flush current in-memory db to lamedb5
        except Exception as e:
            _log("%s: flush save failed: %r" % (tag, e))
        refs = _unwanted_from_file()
        if not refs:
            _log("%s: nothing to remove" % tag)
            return
        n = 0
        for ref in refs:
            try:
                db.removeService(eServiceReference(ref))
                n += 1
            except Exception as e:
                _log("%s: removeService(%s) failed: %r" % (tag, ref, e))
        saved = False
        for m in ("saveServicelist", "save"):
            try:
                getattr(db, m)()
                saved = True
                break
            except Exception:
                continue
        _log("%s: removed %d/%d unwanted services, saved=%s" % (tag, n, len(refs), saved))
    except Exception as e:
        _log("%s: cleanup error: %r" % (tag, e))


def _fix_bouquet(tag):
    """重写 LastScanned bouquet，去掉非电视条目，并重载 bouquet。"""
    try:
        changed_any = False
        for path in glob.glob("/etc/enigma2/userbouquet.*[Ll]ast[Ss]cann*.tv"):
            try:
                lines = open(path).read().splitlines()
            except Exception:
                continue
            out, drop_next = [], False
            changed = False
            for ln in lines:
                if drop_next and ln.startswith("#DESCRIPTION"):
                    drop_next = False
                    changed = True
                    continue
                drop_next = False
                if ln.startswith("#SERVICE 1:0:"):
                    try:
                        stype = int(ln.split(":")[2], 16)
                    except (IndexError, ValueError):
                        stype = 1
                    if stype != 1:
                        drop_next = True
                        changed = True
                        continue
                out.append(ln)
            if changed:
                with open(path, "w") as f:
                    f.write("\n".join(out) + "\n")
                changed_any = True
                _log("%s: cleaned %s" % (tag, path))
        if changed_any:
            from enigma import eDVBDB
            try:
                eDVBDB.getInstance().reloadBouquets()
                _log("%s: bouquets reloaded" % tag)
            except Exception as e:
                _log("%s: reloadBouquets failed: %r" % (tag, e))
    except Exception as e:
        _log("%s: bouquet fix error: %r" % (tag, e))


def _schedule(ms, fn):
    from enigma import eTimer
    t = eTimer()
    t.callback.append(fn)
    t.start(ms, True)
    _timers.append(t)
    if len(_timers) > 8:
        del _timers[:-8]


def _patch():
    from Components.ServiceScan import ServiceScan as CompScan
    import Screens.ServiceScan as ScreenScan

    orig_status = CompScan.scanStatusChanged
    orig_newservice = CompScan.newService
    orig_ok = ScreenScan.ServiceScan.ok

    probed = [0]

    def newService(self):
        if probed[0] < 4:
            try:
                ref = self.scan.getLastServiceRef()
                name = self.scan.getLastServiceName()
                _log("probe: ref=%r name=%r" % (ref, name))
            except Exception as e:
                _log("probe error: %r" % e)
            probed[0] += 1
        return orig_newservice(self)

    def scanStatusChanged(self):
        r = orig_status(self)
        try:
            if not getattr(self, "_sf_done", False):
                scan = getattr(self, "scan", None)
                if scan is not None and scan.isDone():
                    self._sf_done = True
                    _schedule(1500, lambda: _db_cleanup("scan+1.5s"))
                    _schedule(10000, lambda: _db_cleanup("scan+10s"))
                    _log("scan done, cleanup scheduled")
        except Exception as e:
            _log("hook error: %r" % e)
        return r

    def ok(self):
        r = orig_ok(self)
        try:
            # C++ eComponentScan 在结果页关闭时才把扫描结果插入数据库并存盘，
            # 所以清理必须排在其后：先洗 bouquet，再洗数据库。
            _schedule(1500, lambda: _fix_bouquet("ok+1.5s"))
            _schedule(2500, lambda: _db_cleanup("ok+2.5s"))
            _schedule(15000, lambda: _db_cleanup("ok+15s"))
        except Exception as e:
            _log("ok hook error: %r" % e)
        return r

    CompScan.scanStatusChanged = scanStatusChanged
    CompScan.newService = newService
    ScreenScan.ServiceScan.ok = ok
    _log("scan filter v4 active")


def autostart(reason, **kwargs):
    if reason == 0:
        try:
            _patch()
        except Exception as e:
            _log("patch failed: %r" % e)


from Plugins.Plugin import PluginDescriptor


def Plugins(**kwargs):
    return [PluginDescriptor(name="ScanFilter",
                             description="扫描时自动屏蔽非电视服务",
                             where=PluginDescriptor.WHERE_AUTOSTART,
                             fnc=autostart)]
