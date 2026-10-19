package com.womic.open

import android.app.Activity
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.text.Editable
import android.text.TextWatcher
import android.util.Log
import android.view.View
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.ProgressBar
import android.widget.Spinner
import android.widget.TextView
import androidx.core.content.ContextCompat

/**
 * MainActivity -- 采集端 UI。
 *
 * 职责：
 *   - 编辑目标 IP / 端口 / 采样率 / 帧长 / 码率 / 带内 FEC，并持久化到 [Prefs]；
 *   - 广播发现服务端（[Discovery]）并一键回填；
 *   - 启动 / 停止 [MicCaptureService]（前台服务）；
 *   - 显示电平条（RMS / peak）与运行状态、已发包数。
 *
 * UI 与服务的通信方式：绑定服务 + [MicCaptureService.CaptureStatusListener] 回调，
 * 回调均在主线程执行，可直接操作 View。
 */
class MainActivity : Activity() {

    companion object {
        private const val TAG = "MainActivity"
        private const val UI_REFRESH_INTERVAL_MS = 500L
    }

    private lateinit var prefs: Prefs

    private lateinit var editHost: EditText
    private lateinit var editPort: EditText
    private lateinit var spinnerRate: Spinner
    private lateinit var spinnerFrame: Spinner
    private lateinit var spinnerBitrate: Spinner
    private lateinit var checkFec: CheckBox
    private lateinit var buttonStart: Button
    private lateinit var buttonStop: Button
    private lateinit var buttonDiscover: Button
    private lateinit var progressLevel: ProgressBar
    private lateinit var progressPeak: ProgressBar
    private lateinit var textLevel: TextView
    private lateinit var textStatus: TextView
    private lateinit var textDetail: TextView

    private val mainHandler = Handler(Looper.getMainLooper())

    /** 已绑定的服务实例；未绑定时为 null。 */
    private var service: MicCaptureService? = null

    private var bound = false

    private val serviceConnection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
            val svc = (binder as? MicCaptureService.LocalBinder)?.getService()
            service = svc
            bound = svc != null
            svc?.setStatusListener(statusListener)
            refreshUi()
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            service = null
            bound = false
            refreshUi()
        }
    }

    private val statusListener = object : MicCaptureService.CaptureStatusListener {
        override fun onState(state: String, detail: String) {
            textStatus.text = getString(R.string.status_label, state)
            textDetail.text = detail
            refreshUi()
        }

        override fun onLevel(rmsDb: Float, peakDb: Float) {
            progressLevel.progress = LevelMeter.levelToProgress(LevelMeter.dbToLevel(rmsDb))
            progressPeak.progress = LevelMeter.levelToProgress(LevelMeter.dbToLevel(peakDb))
            textLevel.text = getString(R.string.level_label, rmsDb, peakDb)
        }

        override fun onError(message: String) {
            textStatus.text = getString(R.string.status_label, MicCaptureService.STATE_ERROR)
            textDetail.text = message
            refreshUi()
        }
    }

    /** 定时刷新统计（包数/字节数）。 */
    private val uiRefreshRunnable = object : Runnable {
        override fun run() {
            val svc = service
            if (svc != null && MicCaptureService.isRunning) {
                textDetail.text = getString(
                    R.string.detail_running,
                    editHost.text.toString(),
                    currentPort(),
                    svc.packetsSent(),
                    svc.bytesSent() / 1024L
                )
            }
            mainHandler.postDelayed(this, UI_REFRESH_INTERVAL_MS)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        // 启动自检（缺陷 D-11）：默认值必须与 build.gradle.kts 的 BuildConfig 一致
        // 且落在合法区间，否则直接崩在启动阶段，而不是"连上了却没声音"。
        Prefs.verifyDefaults()

        prefs = Prefs(this)

        editHost = findViewById(R.id.edit_host)
        editPort = findViewById(R.id.edit_port)
        spinnerRate = findViewById(R.id.spinner_rate)
        spinnerFrame = findViewById(R.id.spinner_frame)
        spinnerBitrate = findViewById(R.id.spinner_bitrate)
        checkFec = findViewById(R.id.check_fec)
        buttonStart = findViewById(R.id.button_start)
        buttonStop = findViewById(R.id.button_stop)
        buttonDiscover = findViewById(R.id.button_discover)
        progressLevel = findViewById(R.id.progress_level)
        progressPeak = findViewById(R.id.progress_peak)
        textLevel = findViewById(R.id.text_level)
        textStatus = findViewById(R.id.text_status)
        textDetail = findViewById(R.id.text_detail)

        progressLevel.max = 1000
        progressPeak.max = 1000
        progressLevel.progress = 0
        progressPeak.progress = 0

        setupSpinners()
        bindConfigToUi(prefs.load())

        buttonStart.setOnClickListener { onStartClicked() }
        buttonStop.setOnClickListener { onStopClicked() }
        buttonDiscover.setOnClickListener { onDiscoverClicked() }

        // 参数改动即落盘（不自动重连，避免误触）
        val watcher = object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) = Unit
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) = Unit
            override fun afterTextChanged(s: Editable?) {
                prefs.save(readConfigFromUi())
            }
        }
        editHost.addTextChangedListener(watcher)
        editPort.addTextChangedListener(watcher)
        checkFec.setOnCheckedChangeListener { _, _ -> prefs.save(readConfigFromUi()) }

        textDetail.text = getString(R.string.opus_version, OpusEncoder.libraryVersion())
        if (!OpusEncoder.isLoaded()) {
            textStatus.text = getString(R.string.status_label, MicCaptureService.STATE_ERROR)
            textDetail.text = getString(R.string.opus_not_loaded, OpusEncoder.loadErrorMessage() ?: "")
        }
    }

    override fun onStart() {
        super.onStart()
        bindService(Intent(this, MicCaptureService::class.java), serviceConnection, Context.BIND_AUTO_CREATE)
    }

    override fun onResume() {
        super.onResume()
        mainHandler.post(uiRefreshRunnable)
        refreshUi()
    }

    override fun onPause() {
        mainHandler.removeCallbacks(uiRefreshRunnable)
        super.onPause()
    }

    override fun onStop() {
        service?.setStatusListener(null)
        if (bound) {
            try {
                unbindService(serviceConnection)
            } catch (t: Throwable) {
                Log.w(TAG, "unbindService 失败：${t.message}")
            }
            bound = false
        }
        service = null
        super.onStop()
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        when (requestCode) {
            Permissions.REQ_CAPTURE -> {
                if (Permissions.handleResult(requestCode, permissions, grantResults)) {
                    startCaptureService()
                } else {
                    textStatus.text = getString(R.string.status_label, MicCaptureService.STATE_ERROR)
                    textDetail.text = getString(R.string.permission_denied, Permissions.describeMissing(this))
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // UI 装配
    // ------------------------------------------------------------------
    private fun setupSpinners() {
        spinnerRate.adapter = ArrayAdapter(
            this,
            android.R.layout.simple_spinner_item,
            Prefs.SUPPORTED_RATES.map { "$it Hz" }
        ).apply { setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item) }

        spinnerFrame.adapter = ArrayAdapter(
            this,
            android.R.layout.simple_spinner_item,
            Prefs.SUPPORTED_FRAME_MS.map { "$it ms" }
        ).apply { setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item) }

        spinnerBitrate.adapter = ArrayAdapter(
            this,
            android.R.layout.simple_spinner_item,
            Prefs.SUPPORTED_BITRATES.map { "${it / 1000} kbps" }
        ).apply { setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item) }

        val saveListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                prefs.save(readConfigFromUi())
                updateFrameHint()
            }

            override fun onNothingSelected(parent: AdapterView<*>?) = Unit
        }
        spinnerRate.onItemSelectedListener = saveListener
        spinnerFrame.onItemSelectedListener = saveListener
        spinnerBitrate.onItemSelectedListener = saveListener
    }

    private fun bindConfigToUi(config: Prefs.Config) {
        editHost.setText(config.host)
        editPort.setText(config.port.toString())
        spinnerRate.setSelection(Prefs.SUPPORTED_RATES.indexOf(config.sampleRate).coerceAtLeast(0))
        spinnerFrame.setSelection(Prefs.SUPPORTED_FRAME_MS.indexOf(config.frameMs).coerceAtLeast(0))
        spinnerBitrate.setSelection(Prefs.SUPPORTED_BITRATES.indexOf(config.bitrateBps).coerceAtLeast(0))
        checkFec.isChecked = config.fecEnabled
        updateFrameHint()
    }

    /** 把当前 UI 选择读成配置对象（带兜底，避免越界崩溃）。 */
    private fun readConfigFromUi(): Prefs.Config {
        val rate = Prefs.SUPPORTED_RATES.getOrNull(spinnerRate.selectedItemPosition)
            ?: Prefs.DEFAULT_SAMPLE_RATE
        val frame = Prefs.SUPPORTED_FRAME_MS.getOrNull(spinnerFrame.selectedItemPosition)
            ?: Prefs.DEFAULT_FRAME_MS
        val bitrate = Prefs.SUPPORTED_BITRATES.getOrNull(spinnerBitrate.selectedItemPosition)
            ?: Prefs.DEFAULT_BITRATE_BPS
        return Prefs.Config(
            host = editHost.text.toString().trim(),
            port = currentPort(),
            sampleRate = rate,
            frameMs = frame,
            bitrateBps = bitrate,
            fecEnabled = checkFec.isChecked
        )
    }

    private fun currentPort(): Int {
        val raw = editPort.text.toString().trim().toIntOrNull()
        return if (raw == null || raw !in 1..65535) Prefs.DEFAULT_PORT else raw
    }

    /** 帧长提示：显示每包 PCM 帧数，并提示必须与服务端一致。 */
    private fun updateFrameHint() {
        val config = readConfigFromUi()
        val frames = config.framesPerPacket()
        val valid = OpusEncoder.isValidFrame(config.sampleRate, frames)
        val hint = findViewById<TextView>(R.id.text_hint)
        hint.text = if (valid) {
            getString(R.string.hint_frame_ok, config.sampleRate, config.frameMs, frames)
        } else {
            getString(R.string.hint_frame_bad, config.sampleRate, config.frameMs)
        }
    }

    private fun refreshUi() {
        val running = MicCaptureService.isRunning
        buttonStart.isEnabled = !running
        buttonStop.isEnabled = running
        spinnerRate.isEnabled = !running
        spinnerFrame.isEnabled = !running
        spinnerBitrate.isEnabled = !running
        editHost.isEnabled = !running
        editPort.isEnabled = !running
        checkFec.isEnabled = !running
        if (!running) {
            progressLevel.progress = 0
            progressPeak.progress = 0
        }
    }

    // ------------------------------------------------------------------
    // 动作
    // ------------------------------------------------------------------
    private fun onStartClicked() {
        val config = readConfigFromUi()
        if (config.host.isBlank()) {
            textDetail.text = getString(R.string.error_empty_host)
            return
        }
        if (!config.isValid()) {
            textDetail.text = getString(R.string.error_invalid_config)
            return
        }
        prefs.save(config)
        if (!Permissions.allGranted(this)) {
            Permissions.requestIfNeeded(this)
            return
        }
        startCaptureService()
    }

    private fun startCaptureService() {
        val config = readConfigFromUi()
        val intent = MicCaptureService.buildStartIntent(this, config)
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                // Android 8+ 后台启动前台服务必须用 startForegroundService
                ContextCompat.startForegroundService(this, intent)
            } else {
                startService(intent)
            }
        } catch (t: Throwable) {
            Log.e(TAG, "启动服务失败", t)
            textStatus.text = getString(R.string.status_label, MicCaptureService.STATE_ERROR)
            textDetail.text = getString(R.string.error_start_service, t.message ?: "")
            return
        }
        // 绑定以便拿到状态回调
        if (!bound) {
            bindService(Intent(this, MicCaptureService::class.java), serviceConnection, Context.BIND_AUTO_CREATE)
        }
        textStatus.text = getString(R.string.status_label, MicCaptureService.STATE_STARTING)
        refreshUi()
    }

    private fun onStopClicked() {
        try {
            startService(MicCaptureService.buildStopIntent(this))
        } catch (t: Throwable) {
            Log.w(TAG, "停止服务失败：${t.message}")
        }
        progressLevel.progress = 0
        progressPeak.progress = 0
        textStatus.text = getString(R.string.status_label, MicCaptureService.STATE_STOPPED)
        refreshUi()
    }

    /**
     * 广播发现：网络操作，必须放到子线程；结果回主线程更新 UI。
     * 第一个结果直接回填 IP + 端口。
     */
    private fun onDiscoverClicked() {
        buttonDiscover.isEnabled = false
        textDetail.text = getString(R.string.discovering)
        val thread = Thread {
            val discovery = Discovery()
            val servers = discovery.scan(Discovery.DEFAULT_DISCOVERY_PORT, Discovery.DEFAULT_TIMEOUT_MS)
            mainHandler.post {
                buttonDiscover.isEnabled = true
                if (servers.isEmpty()) {
                    textDetail.text = getString(
                        R.string.discover_none,
                        discovery.lastError ?: getString(R.string.discover_hint)
                    )
                    return@post
                }
                val first = servers.first()
                editHost.setText(first.address.hostAddress ?: "")
                editPort.setText(first.audioPort.toString())
                // 端点采样率若不在 Opus 可选集合内（44100），按最接近的可选值回落
                val rate = if (first.sampleRate in Prefs.SUPPORTED_RATES) {
                    first.sampleRate
                } else {
                    Prefs.DEFAULT_SAMPLE_RATE
                }
                spinnerRate.setSelection(Prefs.SUPPORTED_RATES.indexOf(rate).coerceAtLeast(0))
                prefs.save(readConfigFromUi())
                textDetail.text = getString(R.string.discover_found, servers.joinToString("\n") { it.toString() })
            }
        }
        thread.name = "womic-discovery"
        thread.isDaemon = true
        thread.start()
    }
}
