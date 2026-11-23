package com.ghostlock.manager

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.pm.PackageManager
import android.os.Bundle
import android.os.Process
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import rikka.shizuku.Shizuku
import java.io.File

class MainActivity : AppCompatActivity() {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private lateinit var tvStatus: TextView
    private lateinit var tvOut: TextView
    private lateinit var etCmd: EditText
    @Volatile private var starting = false

    private val permissionListener =
        Shizuku.OnRequestPermissionResultListener { requestCode, grantResult ->
            if (requestCode == REQ_SHIZUKU) {
                runOnUiThread {
                    if (grantResult == PackageManager.PERMISSION_GRANTED) startRootViaShizuku()
                    else toast("Shizuku 授权被拒绝")
                }
            }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        tvStatus = findViewById(R.id.tvStatus)
        tvOut = findViewById(R.id.tvOut)
        etCmd = findViewById(R.id.etCmd)

        findViewById<Button>(R.id.btnRefresh).setOnClickListener { refreshStatus() }
        findViewById<Button>(R.id.btnRun).setOnClickListener { runTerminal() }
        findViewById<Button>(R.id.btnClear).setOnClickListener { tvOut.text = "" }
        findViewById<Button>(R.id.btnAuth).setOnClickListener { showAuthDialog() }
        findViewById<Button>(R.id.btnLogs).setOnClickListener { showLogs() }
        findViewById<Button>(R.id.btnStart).setOnClickListener { startRoot() }

        try {
            Shizuku.addRequestPermissionResultListener(permissionListener)
        } catch (_: Throwable) {
        }
        refreshStatus()
    }

    override fun onDestroy() {
        try {
            Shizuku.removeRequestPermissionResultListener(permissionListener)
        } catch (_: Throwable) {
        }
        scope.cancel()
        super.onDestroy()
    }

    // ---- root 调用（经 /data/local/tmp/gl-su -> @ghostlock 服务） ----

    private data class SuResult(val code: Int, val out: String)

    private fun suRun(cmd: String): SuResult {
        if (!File(GSU_PATH).exists()) return SuResult(-1, "gl-su 未部署：$GSU_PATH")
        return try {
            val p = ProcessBuilder(GSU_PATH, "-c", cmd).redirectErrorStream(true).start()
            val out = p.inputStream.bufferedReader().use { it.readText() }
            SuResult(p.waitFor(), out)
        } catch (t: Throwable) {
            SuResult(-1, "执行失败: ${t.message}")
        }
    }

    private fun refreshStatus() {
        scope.launch {
            val deployed = File(GSU_PATH).exists()
            val r = if (deployed) suRun("id -u") else SuResult(-1, "")
            val text = when {
                !deployed -> "未部署\n$GSU_PATH 不存在（需要先启动 root 服务）"
                r.out.contains("not authorized") ->
                    "服务在线，但本应用未授权\nuid=${Process.myUid()} 需加入 /data/adb/gl_allow"
                r.out.contains("cannot connect") ->
                    if (daemonAlive()) "root 服务未运行（守护进程在线，可点“启动 Root 服务”）"
                    else "root 服务未运行（需先在电脑上执行一次 adb 启动命令）"
                r.code == 0 && r.out.trim() == "0" -> "root 可用（uid 0）"
                else -> "状态未知 rc=${r.code}\n${r.out.take(200)}"
            }
            withContext(Dispatchers.Main) { tvStatus.text = text }
        }
    }

    private fun runTerminal() {
        val cmd = etCmd.text.toString().trim()
        if (cmd.isEmpty()) return
        tvOut.append("\n$ $cmd\n")
        scope.launch {
            val r = suRun(cmd)
            withContext(Dispatchers.Main) {
                tvOut.append(r.out)
                if (!r.out.endsWith("\n")) tvOut.append("\n")
                tvOut.append("[rc=${r.code}]\n")
            }
        }
    }

    private fun showAuthDialog() {
        scope.launch {
            val r = suRun("cat /data/adb/gl_allow 2>/dev/null")
            if (r.out.contains("not authorized") || r.code != 0) {
                withContext(Dispatchers.Main) {
                    toast("需要先授权本应用 uid=${Process.myUid()}（可先用 MT/adb 写入 /data/adb/gl_allow）")
                }
                return@launch
            }
            val allowed = r.out.lineSequence().mapNotNull { it.trim().toIntOrNull() }.toSet()
            val pm = packageManager
            val apps = pm.getInstalledPackages(0)
                .mapNotNull { it.applicationInfo }
                .filter { it.uid >= 10000 }
                .sortedBy { pm.getApplicationLabel(it).toString() }
            val labels = apps.map {
                "${pm.getApplicationLabel(it)} (${it.packageName}, uid ${it.uid})"
            }.toTypedArray()
            val checked = apps.map { allowed.contains(it.uid) }.toBooleanArray()

            withContext(Dispatchers.Main) {
                AlertDialog.Builder(this@MainActivity)
                    .setTitle("授权应用（uid 白名单）")
                    .setMultiChoiceItems(labels, checked) { _, which, isChecked ->
                        checked[which] = isChecked
                    }
                    .setPositiveButton("保存") { _, _ ->
                        val uids = LinkedHashSet<Int>()
                        uids.add(2000)
                        apps.forEachIndexed { i, ai ->
                            if (checked[i]) uids.add(ai.uid)
                        }
                        // 保留白名单里已不在安装列表中的 uid（如已卸载应用）
                        allowed.filter { au ->
                            au != 2000 && apps.none { it.uid == au }
                        }.forEach { uids.add(it) }
                        val content = uids.joinToString("\n") + "\n"
                        scope.launch {
                            val w = suRun("printf '%s' '$content' > /data/adb/gl_allow && cat /data/adb/gl_allow")
                            withContext(Dispatchers.Main) {
                                toast(if (w.code == 0) "已保存" else "保存失败: ${w.out.take(120)}")
                            }
                            refreshStatus()
                        }
                    }
                    .setNegativeButton("取消", null)
                    .show()
            }
        }
    }

    private fun showLogs() {
        scope.launch {
            val dir = ghostDir()
            val dstat = try { File(dir, "glstatus").readText().trim() } catch (_: Throwable) { "-" }
            val dlog = try {
                File(dir, "gldaemon.log").readLines().takeLast(80).joinToString("\n")
            } catch (_: Throwable) { "(无守护进程日志)" }
            val r = suRun(
                "echo '== glsrv.log =='; tail -n 120 /data/local/tmp/glsrv.log 2>&1; " +
                    "echo; echo '== rt.log =='; tail -n 60 /data/local/tmp/rt.log 2>&1"
            )
            val text = "daemon status: $dstat\n\n== gldaemon.log ==\n$dlog\n\n" + r.out
            withContext(Dispatchers.Main) {
                AlertDialog.Builder(this@MainActivity)
                    .setTitle("日志")
                    .setMessage(text.ifEmpty { "无输出（rc=${r.code}）" })
                    .setPositiveButton("关闭", null)
                    .show()
            }
        }
    }

    // ---- Shizuku 启动 ----

    private fun shizukuReady(): Boolean = try {
        Shizuku.pingBinder()
    } catch (_: Throwable) {
        false
    }

    /* 旧 Shizuku 拉起源码保留，但按钮已改走 adb 守护进程（keyring 限制）。 */

    /* Shizuku API 13 把 newProcess 改成了 private；这里反射调用（同版本库，非系统隐藏 API） */
    private fun shizukuNewProcess(cmd: Array<String>): java.lang.Process {
        val strArr = Class.forName("[Ljava.lang.String;")
        val m = Shizuku::class.java.getDeclaredMethod(
            "newProcess", strArr, strArr, String::class.java
        )
        m.isAccessible = true
        return m.invoke(null, cmd, null, null) as java.lang.Process
    }

    // ---- adb 守护进程 + 触发文件（Android 10：Shizuku shell 拿不到 fscrypt keyring） ----

    private fun ghostDir(): File = File(getExternalFilesDir(null) ?: filesDir, "ghost")

    private fun daemonAlive(): Boolean {
        return try {
            val ts = File(ghostDir(), "glheartbeat").readText().trim().toLongOrNull()
                ?: return false
            val now = System.currentTimeMillis() / 1000
            now - ts in 0..30
        } catch (_: Throwable) {
            false
        }
    }

    private fun startRoot() {
        if (starting) {
            toast("启动进行中…")
            return
        }
        scope.launch {
            try {
                val dir = ghostDir()
                dir.mkdirs()
                extractAssets(dir)   // 确保守护进程能同步到最新二进制
                val r0 = suRun("id -u")
                if (r0.code == 0 && r0.out.trim() == "0") {
                    withContext(Dispatchers.Main) {
                        toast("root 服务已在运行")
                        refreshStatus()
                    }
                    return@launch
                }
                if (!daemonAlive()) {
                    withContext(Dispatchers.Main) { showDaemonHelp() }
                    return@launch
                }
                starting = true
                File(dir, "gltrigger").writeText("start\n")
                withContext(Dispatchers.Main) { tvStatus.text = "启动中…（等待系统空闲）" }
                var last = ""
                val deadline = System.currentTimeMillis() + 420_000
                while (System.currentTimeMillis() < deadline) {
                    delay(2000)
                    val st = try {
                        File(dir, "glstatus").readText().trim()
                    } catch (_: Throwable) {
                        ""
                    }
                    if (st != last) {
                        last = st
                        val msg = when {
                            st.startsWith("settling") -> "启动中…（等待系统空闲）"
                            st.startsWith("serving") -> "启动中…（启动服务）"
                            st.startsWith("exploit_failed") -> "exploit 失败，可再试一次"
                            st.startsWith("serve_failed") -> "服务启动失败，查看日志"
                            st.startsWith("ready") -> "root 服务已就绪"
                            else -> "启动中…（${st.ifEmpty { "等待守护进程" }}）"
                        }
                        withContext(Dispatchers.Main) { tvStatus.text = msg }
                    }
                    if (st.startsWith("ready") || st.startsWith("exploit_failed") ||
                        st.startsWith("serve_failed")) break
                }
                refreshStatus()
            } finally {
                starting = false
            }
        }
    }

    private fun showDaemonHelp() {
        val dir = ghostDir().absolutePath
        val script = File(ghostDir(), "glboot.sh").absolutePath
        val cmd = "adb shell setsid nohup sh $script $dir >/dev/null 2>&1 </dev/null &"
        AlertDialog.Builder(this)
            .setTitle("需要先启动 adb 守护进程")
            .setMessage(
                "实测在 Android 10 上，Shizuku 的 shell 会话拿不到 fscrypt keyring" +
                    "（只能看到、无法链接），root exploit 必须由 adb 会话启动的守护进程执行。\n\n" +
                    "每次开机后在电脑上执行一次（和 Shizuku 本身的开机要求一样）：\n\n$cmd\n\n" +
                    "守护进程起来后，回到 App 点“重新检测”，之后启动 Root 就是 App 内一键。"
            )
            .setPositiveButton("复制命令") { _, _ ->
                val cm = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                cm.setPrimaryClip(ClipData.newPlainText("glboot", cmd))
                toast("已复制，粘贴到电脑的 adb 里执行")
            }
            .setNeutralButton("重新检测") { _, _ -> startRoot() }
            .setNegativeButton("关闭", null)
            .show()
    }

    private fun startRootViaShizuku() {
        scope.launch {
            try {
                val dir = File(getExternalFilesDir(null) ?: filesDir, "ghost")
                dir.mkdirs()
                extractAssets(dir)
                val script = File(dir, "start_root.sh").absolutePath
                val p = shizukuNewProcess(
                    arrayOf("/system/bin/sh", script, dir.absolutePath)
                )
                val out = p.inputStream.bufferedReader().use { it.readText() }
                val rc = p.waitFor()
                withContext(Dispatchers.Main) {
                    tvOut.append("\n== start_root.sh rc=$rc ==\n$out\n")
                    refreshStatus()
                }
            } catch (t: Throwable) {
                withContext(Dispatchers.Main) { toast("启动失败: ${t.message}") }
            }
        }
    }

    private fun extractAssets(dst: File) {
        val names = assets.list("ghost") ?: return
        for (n in names) {
            val f = File(dst, n)
            assets.open("ghost/$n").use { ins ->
                f.outputStream().use { outs -> ins.copyTo(outs) }
            }
            if (n.endsWith(".sh")) f.setExecutable(true)
        }
    }

    private fun toast(s: String) = Toast.makeText(this, s, Toast.LENGTH_LONG).show()

    companion object {
        private const val GSU_PATH = "/data/local/tmp/gl-su"
        private const val REQ_SHIZUKU = 1001
    }
}