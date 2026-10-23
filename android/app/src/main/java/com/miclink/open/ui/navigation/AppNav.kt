package com.miclink.open.ui.navigation

import androidx.activity.compose.BackHandler
import androidx.annotation.StringRes
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.Menu
import androidx.compose.material.icons.filled.Palette
import androidx.compose.material3.DrawerValue
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalDrawerSheet
import androidx.compose.material3.ModalNavigationDrawer
import androidx.compose.material3.NavigationDrawerItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.rememberDrawerState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import com.miclink.open.MainViewModel
import com.miclink.open.R
import com.miclink.open.ui.screen.AboutScreen
import com.miclink.open.ui.screen.AppearanceScreen
import com.miclink.open.ui.screen.HomeScreen
import kotlinx.coroutines.launch

/** 抽屉导航的三个目的地。 */
private enum class Dest(
    val route: String,
    @param:StringRes val labelRes: Int,
    val icon: ImageVector
) {
    HOME("home", R.string.nav_home, Icons.Filled.Home),
    APPEARANCE("appearance", R.string.nav_appearance, Icons.Filled.Palette),
    ABOUT("about", R.string.nav_about, Icons.Filled.Info)
}

/**
 * AppNav -- 横向抽屉导航 + 三目的地（主页 / 外观 / 关于与帮助）。
 *
 * 交互约束（验收 #4）：
 *  1. 抽屉支持**边缘滑出**（ModalNavigationDrawer 默认手势），也可点左上角菜单按钮；
 *  2. 返回键**优先关抽屉**：抽屉展开时 BackHandler 拦截返回，先收抽屉，
 *     不直接回退导航栈或退出 App；
 *  3. 三个目的地用 `popUpTo(HOME){saveState} + restoreState` 做扁平切换：
 *     不会无限堆栈，且主页滚动位置等状态在切换后能恢复。
 *
 * Scaffold 放在抽屉内容区内（而不是包着抽屉）：TopAppBar 只属于当前页面，
 * 抽屉拉出时盖住的是整个页面含顶栏，符合 Material 3 模态抽屉语义。
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AppNav(
    vm: MainViewModel,
    onStartClicked: () -> Unit,
    onStopClicked: () -> Unit
) {
    val navController = rememberNavController()
    val drawerState = rememberDrawerState(DrawerValue.Closed)
    val scope = rememberCoroutineScope()

    val backStackEntry by navController.currentBackStackEntryAsState()
    val currentRoute = backStackEntry?.destination?.route ?: Dest.HOME.route
    val currentDest = Dest.entries.firstOrNull { it.route == currentRoute } ?: Dest.HOME

    // 返回键先关抽屉（验收 #4）
    BackHandler(enabled = drawerState.isOpen) {
        scope.launch { drawerState.close() }
    }

    ModalNavigationDrawer(
        drawerState = drawerState,
        gesturesEnabled = true,
        drawerContent = {
            ModalDrawerSheet {
                Spacer(Modifier.height(12.dp))
                Text(
                    text = stringResource(R.string.app_name),
                    style = MaterialTheme.typography.titleLarge,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp)
                )
                HorizontalDivider()
                Spacer(Modifier.height(8.dp))
                Dest.entries.forEach { dest ->
                    NavigationDrawerItem(
                        icon = { Icon(dest.icon, contentDescription = null) },
                        label = { Text(stringResource(dest.labelRes)) },
                        selected = dest == currentDest,
                        onClick = {
                            scope.launch { drawerState.close() }
                            if (dest.route != currentRoute) {
                                navController.navigate(dest.route) {
                                    popUpTo(Dest.HOME.route) { saveState = true }
                                    launchSingleTop = true
                                    restoreState = true
                                }
                            }
                        },
                        modifier = Modifier.padding(horizontal = 12.dp)
                    )
                }
            }
        }
    ) {
        Scaffold(
            topBar = {
                TopAppBar(
                    title = { Text(stringResource(currentDest.labelRes)) },
                    navigationIcon = {
                        IconButton(onClick = { scope.launch { drawerState.open() } }) {
                            Icon(
                                imageVector = Icons.Filled.Menu,
                                contentDescription = stringResource(R.string.nav_open_drawer)
                            )
                        }
                    }
                )
            }
        ) { innerPadding ->
            NavHost(
                navController = navController,
                startDestination = Dest.HOME.route,
                modifier = Modifier.padding(innerPadding)
            ) {
                composable(Dest.HOME.route) {
                    HomeScreen(
                        vm = vm,
                        onStartClicked = onStartClicked,
                        onStopClicked = onStopClicked
                    )
                }
                composable(Dest.APPEARANCE.route) {
                    AppearanceScreen(vm = vm)
                }
                composable(Dest.ABOUT.route) {
                    AboutScreen()
                }
            }
        }
    }
}
