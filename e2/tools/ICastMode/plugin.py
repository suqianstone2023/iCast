# -*- coding: utf-8 -*-
#
# ICastMode - choose the delivery system of the SMIT iCast USB stick from the GUI.
#
# The stick carries one demodulator, so it is either DVB-C or DTMB / DVB-T,
# never both at the same time.  The choice is stored in /etc/icast.mode, which
# /usr/bin/icast-boot.sh reads at boot in order to insmod the driver with the
# matching icast_mode=... parameter.
#
# enigma2 reads the frontend type (FE_GET_INFO) once while starting up, so a
# change only becomes visible after a full reboot - hence the reboot prompt.

import os

from Screens.Screen import Screen
from Screens.MessageBox import MessageBox
from Screens.Standby import TryQuitMainloop
from Components.ActionMap import ActionMap
from Components.Label import Label
from Components.MenuList import MenuList
from Plugins.Plugin import PluginDescriptor

MODE_FILE = "/etc/icast.mode"
SYSFS_MODE = "/sys/module/dvb_usb_icast_smit/parameters/icast_mode"

MODES = (
	("dvbc", "DVB-C   有线"),
	("dtmb", "DTMB / DVB-T   地面波"),
)

try:
	print("[ICastMode] plugin registered")
except Exception:
	pass


def read_file(path, default=""):
	try:
		with open(path, "r") as f:
			return f.read().strip()
	except Exception:
		return default


class ICastMode(Screen):
	skin = """
		<screen name="ICastMode" position="center,center" size="760,430" title="iCast 调谐器模式">
			<widget name="status" position="25,20" size="710,110" font="Regular;24" halign="left" valign="top" />
			<widget name="list" position="25,145" size="710,150" scrollbarMode="showOnDemand" />
			<widget name="key_green" position="80,340" size="220,45" font="Regular;22" halign="center" valign="center" foregroundColor="#00ff00" />
			<widget name="key_red" position="420,340" size="220,45" font="Regular;22" halign="center" valign="center" foregroundColor="#ff4040" />
		</screen>
	"""

	def __init__(self, session):
		Screen.__init__(self, session)
		self.session = session
		self["status"] = Label("")
		self["list"] = MenuList([m[1] for m in MODES])
		self["key_green"] = Label("应用并重启")
		self["key_red"] = Label("关闭")
		self["actions"] = ActionMap(["OkCancelActions", "ColorActions"], {
			"ok": self.apply,
			"green": self.apply,
			"cancel": self.close,
			"red": self.close,
		}, -1)
		self.refresh()

	def refresh(self):
		conf = read_file(MODE_FILE, "dvbc") or "dvbc"
		run = read_file(SYSFS_MODE, "驱动未加载")
		self["status"].setText(
			"配置：%s      驱动运行：%s\n\n同一时刻只能用一种模式，切换后需要重启盒子。" % (conf, run)
		)
		for i, m in enumerate(MODES):
			if m[0] == conf:
				try:
					self["list"].moveToIndex(i)
				except Exception:
					pass
				break

	def apply(self):
		try:
			idx = self["list"].getSelectionIndex()
		except Exception:
			return
		if idx < 0 or idx >= len(MODES):
			return
		mode = MODES[idx][0]
		conf = read_file(MODE_FILE, "dvbc") or "dvbc"
		if mode == conf:
			self.session.open(MessageBox, "当前已经是 %s 模式。" % mode,
				MessageBox.TYPE_INFO, timeout=4)
			return
		try:
			with open(MODE_FILE, "w") as f:
				f.write(mode + "\n")
				f.flush()
				os.fsync(f.fileno())
		except Exception as e:
			self.session.open(MessageBox, "写入 %s 失败：\n%s" % (MODE_FILE, e),
				MessageBox.TYPE_ERROR, timeout=8)
			return
		self.refresh()
		self.session.openWithCallback(self.rebootNow, MessageBox,
			"已保存为 %s。\n\nenigma2 只在启动时读取一次调谐器类型，\n"
			"必须重启整个盒子才会生效。\n\n现在重启吗？" % mode,
			MessageBox.TYPE_YESNO, timeout=30)

	def rebootNow(self, answer):
		if answer:
			self.close()
			self.session.open(TryQuitMainloop, 2)


def main(session, **kwargs):
	session.open(ICastMode)


def Plugins(**kwargs):
	d = "选择 iCast 棒子的工作模式（DVB-C / DTMB）"
	return [
		PluginDescriptor(name="iCast 调谐器模式", description=d,
			where=PluginDescriptor.WHERE_PLUGINMENU, fnc=main),
		PluginDescriptor(name="iCast 调谐器模式", description=d,
			where=PluginDescriptor.WHERE_EXTENSIONSMENU, fnc=main),
	]
