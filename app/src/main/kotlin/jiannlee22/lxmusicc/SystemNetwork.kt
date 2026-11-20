package jiannlee22.lxmusicc

import android.content.Context
import android.net.ConnectivityManager
import android.net.NetworkCapabilities

/** 只读取系统事实；下载策略由 C 根据用户设置判断，查询异常按未知网络处理。 */
internal object SystemNetwork {
    fun flags(context: Context): Int = try {
        val manager = context.getSystemService(ConnectivityManager::class.java)
        val network = manager.activeNetwork
        val caps = if (network == null) null else manager.getNetworkCapabilities(network)
        if (caps == null) 0 else {
            (if (caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) 1 else 0) or
                (if (caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED)) 2 else 0) or
                (if (caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED)) 4 else 0)
        }
    } catch (_: Exception) { 0 }
}
