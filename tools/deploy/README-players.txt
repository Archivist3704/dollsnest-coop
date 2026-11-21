Dolls Nest 联机 mod / Co-op Mod
本文件含 简体中文 / English / 日本語 —— This file is in Chinese, English and Japanese.


======================================================================
 简体中文
======================================================================

Dolls Nest 联机 mod

这是什么
  让《Dolls Nest》可以最多四人一起玩的pve mod。由于友伤因为一些不可抗力被我关了，所以一些踹哈种子可以洗洗睡了。

怎么装
  把整个压缩包解压到：

      <你的 Steam 库>\steamapps\common\Dolls Nest\

  解压完成之后，DollsNest\Binaries\Win64\ 里应该多出这几个文件：

      version.dll          （让游戏自己把 mod 加载起来）
      dollsnest-coop.dll   （mod 本体）
      README.txt           （这份说明，留在那里就行）
      peers.txt            （虚拟局域网ip名单）

  然后从 Steam 正常启动游戏。

怎么联机
  1. 双方都装 Radmin VPN（或同类虚拟局域网），加入同一个网络。
  2. 在peers.txt写所有想要联机的人的虚拟局域网ip地址，一行一个，比如：

          114.514.19.810
          67.67.67.67

     所有人都必须写，因为我忘了不写能不能连上了。
  3. 主机进到要玩的那张地图，按 F7，选第一行“担任主机”。
  4. 客机在同一张图按 F7，选第二行“加入房间”。
  5. 两个人必须站在同一张地图上。不同地图不会互相看见，也不会有任何提示。

怎么知道装好了
  - 启动游戏之后，Win64 目录里会出现 dsh-proxy-load.txt，里面最新一行的时间就是刚才。
  - 游戏里按 F7 能弹出菜单，就是加载成功了。

杀毒软件说它是病毒
  这是未签名的 mod，偶尔会被误报（请相信我，我没有往mod里写病毒的本事）。三步：
  1. 打开杀毒软件的隔离区，把被隔离的文件还原；
  2. 把 Win64 这个目录加入排除 / 信任列表；
  3. 重新启动游戏。

卸载
  删掉Win64目录里除了DollsNest-Win64-Shipping.exe外的所有文件就行，游戏会回到原样。

限制
- 只有同图才能配对；换地图会结束联机。
- 在休息点补给、或者死亡，都会结束会话 —— 之后重新按 F7 担任主机或加入即可。
- 主机结束会话时客机不会收到提示，需要自己退出。
- 实测过 2 人和 3 人，第 4 人没有测过。
- 主机的boss是否挑战过的状态会写入客机存档，这是为了同步进度。
- 尽管进度本身不影响联机，但还是请尽量让进度较慢的人担任主机。如果进度一致那无所谓。
- 游戏对剧情道具和进度有一套完善的保护措施，但实在怕坏档的话，请备份存档进行联机。


======================================================================
 English
======================================================================

Dolls Nest Co-op Mod

What this is
  A PvE mod that lets up to four people play Dolls Nest together. Friendly fire is off, for
  reasons beyond my control -- so the invaders may as well go to bed.

How to install
  Extract the whole archive into:

      <your Steam library>\steamapps\common\Dolls Nest\

  When that is done, DollsNest\Binaries\Win64\ should have gained these files:

      version.dll          (makes the game load the mod by itself)
      dollsnest-coop.dll   (the mod itself)
      README.txt           (this file -- just leave it there)
      peers.txt            (the list of virtual-LAN addresses)

  Then start the game from Steam as usual.

How to play together
  1. Both players install Radmin VPN (or a similar virtual-LAN tool) and join the same network.
  2. Write the virtual-LAN address of everyone you want to play with into peers.txt, one per
     line, for example:

          114.514.19.810
          67.67.67.67

     Everyone has to be listed -- I have forgotten whether it connects without this.
  3. The host goes into the map they want to play, presses F7 and picks the first line,
     "Host a session".
  4. The client, on the same map, presses F7 and picks the second line, "Join a session".
  5. Both players have to be on the same map. On different maps you will not see each other,
     and nothing tells you why.

How to tell it worked
  - After the game starts, dsh-proxy-load.txt appears in the Win64 folder; the time on its
    newest line should be the time you just launched it.
  - If F7 opens the menu in game, the mod has loaded.

"My antivirus says it is a virus"
  This is an unsigned mod and it does get falsely flagged now and then (believe me, I do not
  have what it takes to write a virus into a mod). Three steps:
  1. Open your antivirus quarantine and restore the quarantined file;
  2. Add the Win64 folder to the exclusions / trusted list;
  3. Start the game again.

Uninstall
  Delete every file in the Win64 folder except DollsNest-Win64-Shipping.exe, and the game is
  back to how it was.

Limits
- Pairing only works on the same map; changing maps ends the session.
- Supplying at a rest point, or dying, ends the session -- press F7 and host or join again.
- When the host ends the session the client is not told, and has to leave the menu itself.
- Tested with 2 and 3 players; the 4th has never been tested.
- The host's boss-cleared state is written into the client's save, so that progress stays in
  step.
- Progress itself does not affect the connection, but prefer letting the player who is behind
  host. If you are at the same point it does not matter.
- The game guards story items and progress carefully, but if you are worried about your save,
  back it up before playing.


======================================================================
 日本語
======================================================================

Dolls Nest 協力mod

これは何か
  『Dolls Nest』を最大 4 人で一緒に遊べるようにする PvE mod です。フレンドリーファイアは諸事情により
  オフにしてあります。なので侵入してくる連中はもう寝て大丈夫です。

インストール
  アーカイブをまるごと次に解凍してください：

      <Steam ライブラリ>\steamapps\common\Dolls Nest\

  解凍が終わると、DollsNest\Binaries\Win64\ に次のファイルが増えているはずです：

      version.dll          （ゲーム自身に mod を読み込ませる）
      dollsnest-coop.dll   （mod 本体）
      README.txt           （この説明。置いたままで構いません）
      peers.txt            （仮想 LAN のアドレス一覧）

  あとは Steam から普通にゲームを起動してください。

一緒に遊ぶ
  1. 双方が Radmin VPN（または同種の仮想 LAN ツール）を入れ、同じネットワークに入ります。
  2. 一緒に遊びたい全員の仮想 LAN アドレスを peers.txt に 1 行 1 人で書きます。例：

          114.514.19.810
          67.67.67.67

     全員分を書いてください。書かなくても繋がるかどうか、私は忘れてしまいました。
  3. ホストは遊びたいマップに入り、F7 を押して 1 行目の「ホストになる」を選びます。
  4. クライアントは同じマップで F7 を押し、2 行目の「セッションに参加」を選びます。
  5. 二人は同じマップにいる必要があります。別のマップでは互いに見えず、何のメッセージも出ません。

うまく入ったか確認する
  - ゲームを起動すると Win64 フォルダに dsh-proxy-load.txt ができ、一番新しい行の時刻が
    たった今起動した時刻になっています。
  - ゲーム中に F7 でメニューが出れば、読み込み成功です。

「ウイルス対策ソフトがウイルスだと言う」
  これは署名のない mod なので、時々誤検出されます（信じてください、mod にウイルスを仕込むような
  腕は私にはありません）。手順は 3 つ：
  1. ウイルス対策ソフトの隔離領域を開き、隔離されたファイルを復元する；
  2. Win64 フォルダを除外／信頼リストに追加する；
  3. ゲームを起動し直す。

アンインストール
  Win64 フォルダ内の DollsNest-Win64-Shipping.exe 以外のファイルをすべて削除すれば、
  ゲームは元の状態に戻ります。

制限
- 同じマップのときだけペアになれます。マップを移動するとセッションは終了します。
- 休憩地点での補給、または死亡でセッションは終了します。その後もう一度 F7 でホスト／参加してください。
- ホストがセッションを終えてもクライアントには通知されません。自分でメニューから抜けてください。
- 2 人と 3 人で確認済み。4 人目は未検証です。
- ホストのボス撃破状態はクライアントのセーブに書き込まれます。進行度を合わせるためです。
- 進行度そのものは接続に影響しませんが、進みの遅い人をホストにするのがおすすめです。揃っていれば
  気にしなくて大丈夫です。
- ストーリーアイテムと進行度には保護が入っていますが、心配ならセーブをバックアップしてから
  遊んでください。
