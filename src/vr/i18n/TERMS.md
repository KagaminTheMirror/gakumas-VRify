# VR 菜单专有名词

改译文时对着填。同一行是同一概念。


| 英文 / 代码                            | 简中                      | 繁中                      | 日语                      | 说明                                            |
| ---------------------------------- | ----------------------- | ----------------------- | ----------------------- | --------------------------------------------- |
| source camera                      | 源相机                     | 來源相機                    | ソースカメラ                  | 仍在跑的那台 2D 摄影相机；Grip 吃它的画面                     |
| authored / shot pose               | 官方机位                    | 官方機位                    | 本編カメラ                   | 官方镜头位姿                                        |
| eye camera / eyes                  | 左右眼                     | 左右眼                     | 左右の目                    | 提交给 HMD 的左右眼相机                                |
| monitor / 2D / display             | 2D 画面                   | 2D 畫面                   | 2D画面                    | 电脑显示器 + Grip 里那张 2D 图                         |
| Grip（画面）                           | Grip                    | Grip                    | Grip                    | 手柄唤出的游戏 UI，底是源相机画面；仅作为开发过程的俗称，不要在菜单等地方暴露给玩家。  |
| game panel                         | 游戏面板                    | 遊戲面板                    | ゲームパネル                  | 游戏内的 2D UI 面板                                 |
| VR menu                            | VR 设置                   | VR 選單                   | VR設定                    | 模组自己的设置菜单                                     |
| headset（Toon 参照）                   | 头显位置                    | 頭戴顯示器位置                 | ヘッドセット位置                | 自由相机里的头显位置，头显位置=玩家位置+头显偏移                     |
| player（Toon 参照）                    | 玩家位置                    | 玩家位置                    | プレイヤー位置                 | 自由相机里的玩家位置                                    |
| free camera                        | 自由相机                    | 自由相機                    | フリーカメラ                  | 模组自由机位模式                                      |
| FOLLOW                             | 跟随                      | 跟隨                      | フォロー                    | 锚在骨骼上的环绕跟随                                    |
| first person                       | 第一人称                    | 第一人稱                    | 一人称                     |                                               |
| render scale                       | 渲染尺度                    | 渲染尺度                    | レンダースケール                | 双眼缓冲的分辨率倍率                                    |
| stereo / both eyes                 | 双眼                      | 雙眼                      | 両眼                      | 左右眼成对                                         |
| eye buffers                        | 双眼缓冲                    | 雙眼緩衝                    | 両眼バッファ                  | 左右眼的渲染目标                                      |
| Grip（键）                            | Grip                    | Grip                    | Grip                    | 控制器 Grip 键                                    |
| trigger                            | 扳机                      | 扳機                      | トリガー                    |                                               |
| stick                              | 摇杆                      | 搖桿                      | スティック                   |                                               |
| cursor smoothing / 1€              | 光标平滑                    | 游標平滑                    | カーソルスムージング              | 与 VD 同族的 1€ 滤波                                |
| Virtual Desktop / VD               | VD                      | VD                      | VD                      | 串流软件                                          |
| VL（`VL.Rendering`）                 | VL                      | VL                      | VL                      | 游戏自带渲染 / 后处理栈                                 |
| bloom / VLBloom                    | 泛光                      | 泛光                      | ブルーム                    | 辉光                                            |
| ProFlare                           | ProFlare                | ProFlare                | ProFlare                | 舞台镜头光斑                                        |
| Toon                               | Toon                    | Toon                    | Toon                    | 角色卡通光影朝向 / 分色                                 |
| shade band                         | 阴影带                     | 陰影帶                     | シェードバンド                 | Toon 亮部 / 暗部分界                                |
| projected actor shadow             | 角色自投影                   | 角色自投影                   | キャラの影                   | 角色自身投下的影子                                     |
| lighting region / volume           | 光照区域                    | 光照區域                    | 照明領域                    | 按机位划分的灯 / 雾 / 后效分区                            |
| anti-aliasing inherit              | 跟随游戏默认设置                | 跟隨遊戲預設                  | ゲームの初期設定に従う            | 眼睛抗锯齿跟源相机走                                    |
| TAA / SMAA / SMAA T2x / TSCMAA     | （原文）                    | （原文）                    | （原文）                    | 模式名即专名                                        |
| TAA Quality 档位                     | Very Low … Very High    | Very Low … Very High    | Very Low … Very High    | URP 枚举名                                       |
| Jitter Scale / Frame Influence     | （原文）                    | （原文）                    | （原文）                    | 菜单仅暴露 TAA 实际消费的字段；Quality 保持 Low          |
| outline                            | 角色描边                    | 角色描邊                    | キャラのアウトライン              |                                               |
| VL paraffin                        | VL paraffin             | VL paraffin             | VL パラフィン                | VL 全屏半透明形状                                    |
| VLTextureBlur                      | VLTextureBlur           | VLTextureBlur           | VLTextureBlur           | 全屏模糊贴图                                        |
| OverlayCanvas (event 600)          | OverlayCanvas           | OverlayCanvas           | OverlayCanvas           | 全屏 UI 卡                                       |
| Live camera overlay                | Live 镜头 overlay         | Live 鏡頭 overlay         | Live カメラオーバーレイ          | 近平面镜头贴图                                       |
| cmov particles                     | cmov 粒子                 | cmov 粒子                 | cmov パーティクル             | 名字 / 路径带 cmov 的 Live 粒子                       |
| camera texture overlay             | 镜头纹理膜                   | 鏡頭紋理膜                   | レンズオーバーレイ               | 上面几类全屏膜的菜单总称                                  |
| VL motion blur                     | VL 运动模糊                 | VL 運動模糊                 | VL モーションブラー             | VL 自带运镜模糊 pass（不是 URP MotionBlur）             |
| deferred punctual / stencil lights | 延迟点光                    | 延遲點光                    | ディファードポイントライト           | 舞台射灯 / 点光的延迟高光                                |
| tiny source / Pause 2D view        | 暂停 2D 画面                | 暫停 2D 畫面                | **2D画面を一時停止**           | 把源相机的 2D 实时画面显示卡在最后一帧，实质后台还跑 tiny 源相机保证游戏正常运行 |
| photo toast: paused 2D             | 暂停 2D 画面时无法拍照         | 暫停 2D 畫面時無法拍照         | 2D画面を停止中は撮影できません     | 右 A 快捷快门在「暂停 2D 画面」开启时被拦下的 toast；Grip 里点官方拍照按钮不拦 |
| transparent panel                  | 面板背景透明化                 | 面板背景透明化                 | パネルを透明に                 | Grip 面板底图                                     |
| glow sticks                        | 荧光棒                     | 螢光棒                     | ペンライト                   |                                               |
| ColorTable                         | 色表                      | 色彩表                     | カラーテーブル                 | Live 观众用的颜色表                                  |
| Live                               | Live                    | Live                    | ライブ                     | 演唱会                                           |
| lobby                              | 大厅                      | 大廳                      | ロビー                     | 进入游戏的第一个默认场景，是进入各个界面的入口                       |
| produce                            | 育成                      | 育成                      | プロデュース                  |                                               |
| 初星コミュ                              | 横屏剧情                    | 橫向劇情                    | 初星コミュ                   | 字幕 + 播放器，无对话框                                 |
| Idol photography                   | 偶像摄影                    | 偶像攝影                    | アイドル撮影                  | 偶像拍照模式                                        |
| character / actor                  | 角色                      | 角色                      | キャラ                     |                                               |
| localizationConfig.json            | localizationConfig.json | localizationConfig.json | localizationConfig.json | 模组配置文件                                        |
| URP                                | URP                     | URP                     | URP                     | Universal Render Pipeline                     |
| OpenXR                             | OpenXR                  | OpenXR                  | OpenXR                  |                                               |
| authored                           | 官方                      | 官方                      | 本編                      | 游戏官方数值 / 观感                                   |


