// SettingsForm and page-container implementation adapted from Diorser/LiteMonitor.
// Upstream commit and license: THIRD_PARTY_NOTICES.md. Pulse bindings below are independent.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;
using LiteMonitor.src.Core;
using LiteMonitor.src.UI.Controls;

namespace LiteMonitor.src.Core {
    public static class UIUtils {
        public static float ScaleFactor=1;
        public static int S(int n) {return (int)(n*ScaleFactor);}
        public static Padding S(Padding p) {return new Padding(S(p.Left),S(p.Top),S(p.Right),S(p.Bottom));}
    }
}
namespace Pulse.SettingsUI {
    sealed class Option {public string Label;public int Command;public bool Checked,Enabled;}
    sealed class Row {public string Label;public int Kind;public List<Option> Options=new List<Option>();}
    sealed class Page {public string Title,Description;public List<Row> Rows=new List<Row>();}
    static class Wire {
        public const int CopyData=0x4a,SelectPage=0x8000+200,FindControl=0x8000+201,Commit=0x8000+202,ScrollCheck=0x8000+203;
        [StructLayout(LayoutKind.Sequential)] public struct Packet {public IntPtr Tag;public int Bytes;public IntPtr Data;}
        [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageTimeout(IntPtr h,int m,IntPtr w,ref Packet p,uint flags,uint timeout,out IntPtr result);
        [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
        [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
        [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h,int message,IntPtr w,IntPtr l);
        [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
        [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h,StringBuilder s,int count);
        public static string String(BinaryReader r) {int n=r.ReadInt32();if(n<0||n>16384)throw new InvalidDataException();var b=r.ReadBytes(n*2);if(b.Length!=n*2)throw new EndOfStreamException();return Encoding.Unicode.GetString(b);}
        public static void String(BinaryWriter w,string s) {w.Write(s.Length);w.Write(Encoding.Unicode.GetBytes(s));}
        public static bool Send(IntPtr target,IntPtr sender,int kind,int command,string text) {
            using(var stream=new MemoryStream())using(var writer=new BinaryWriter(stream)) {
                writer.Write(kind);writer.Write(command);String(writer,text??"");var raw=stream.ToArray();var pin=GCHandle.Alloc(raw,GCHandleType.Pinned);
                try {var p=new Packet{Tag=(IntPtr)602,Bytes=raw.Length,Data=pin.AddrOfPinnedObject()};IntPtr result;return SendMessageTimeout(target,CopyData,sender,ref p,2,2000,out result)!=IntPtr.Zero&&result!=IntPtr.Zero;}finally{pin.Free();}
            }
        }
    }
    sealed class SettingsPage : UserControl {
        public readonly BufferedPanel Container;
        public Page Model;
        public bool Updating;
        readonly Action<int,string> dispatch;
        readonly Dictionary<int,Control> inputs=new Dictionary<int,Control>();
        readonly List<Tuple<Row,Control>> bindings=new List<Tuple<Row,Control>>();
        readonly HashSet<Control> dirty=new HashSet<Control>();
        public SettingsPage(Page page,Action<int,string> send) {
            dispatch=send;Dock=DockStyle.Fill;DoubleBuffered=true;BackColor=UIColors.MainBg;
            // Directly reused from LiteMonitor MainPanelPage: native AutoScroll,
            // buffered container and docked groups, no manual child-window scrolling.
            Container=new BufferedPanel{Dock=DockStyle.Fill,AutoScroll=true,Padding=UIUtils.S(new Padding(20))};Controls.Add(Container);Build(page);
        }
        static bool Same(Page a,Page b) {
            if(a.Title!=b.Title||a.Rows.Count!=b.Rows.Count)return false;
            for(int i=0;i<a.Rows.Count;i++){var x=a.Rows[i];var y=b.Rows[i];if(x.Label!=y.Label||x.Kind!=y.Kind||x.Options.Count!=y.Options.Count)return false;
                for(int j=0;j<x.Options.Count;j++)if((x.Options[j].Label!=y.Options[j].Label&&x.Kind!=4)||x.Options[j].Command!=y.Options[j].Command)return false;}
            return true;
        }
        void AddGroup(LiteSettingsGroup group) {
            var wrapper=new Panel{Dock=DockStyle.Top,AutoSize=true,Padding=UIUtils.S(new Padding(0,0,0,20))};wrapper.Controls.Add(group);
            Container.Controls.Add(wrapper);Container.Controls.SetChildIndex(wrapper,0);
        }
        void Build(Page page) {
            Updating=true;Container.SuspendLayout();
            try {
                foreach(Control c in Container.Controls.Cast<Control>().ToArray()){Container.Controls.Remove(c);c.Dispose();}
                inputs.Clear();bindings.Clear();dirty.Clear();Model=page;LiteSettingsGroup group=null;
                foreach(var row in page.Rows) {
                    if(row.Kind==5){group=new LiteSettingsGroup(row.Label);AddGroup(group);continue;}
                    if(group==null){group=new LiteSettingsGroup(page.Title);AddGroup(group);}
                    if(row.Kind==3){group.AddFullItem(new LiteHintRow(row.Label));continue;}
                    if(row.Options.Count==0)continue;
                    Control input,focus;
                    if(row.Kind==0) {
                        var check=new LiteCheck(row.Options[0].Checked,"");input=focus=check;
                        check.CheckedChanged+=(s,e)=>{if(!Updating)dispatch(row.Options[0].Command,"");};
                    } else if(row.Kind==1) {
                        var combo=new LiteComboBox{Width=UIUtils.S(190)};foreach(var option in row.Options)combo.Items.Add(option.Label);
                        combo.SelectedIndex=row.Options.FindIndex(o=>o.Checked);input=combo;focus=combo.Inner;
                        combo.Inner.SelectedIndexChanged+=(s,e)=>{int index=combo.SelectedIndex;if(!Updating&&index>=0&&row.Options[index].Enabled)dispatch(row.Options[index].Command,"");};
                        combo.Inner.DropDown+=(s,e)=>{int width=combo.Width;foreach(var option in row.Options)width=Math.Max(width,TextRenderer.MeasureText(option.Label,Font).Width+28);combo.Inner.DropDownWidth=width;};
                    } else if(row.Kind==4) {
                        var edit=new LiteUnderlineInput(row.Options[0].Label,"","",240);input=edit;focus=edit.Inner;edit.Inner.MaxLength=96;
                        edit.Inner.TextChanged+=(s,e)=>{if(!Updating)dirty.Add(edit);};
                        edit.Inner.Leave+=(s,e)=>{if(!Updating&&edit.Inner.Text!=row.Options[0].Label)dispatch(row.Options[0].Command,edit.Inner.Text);};
                    } else {
                        var button=new LiteButton(row.Label,false){AutoSize=true,MinimumSize=new Size(UIUtils.S(100),UIUtils.S(32))};input=focus=button;
                        button.Click+=(s,e)=>{if(!Updating)dispatch(row.Options[0].Command,"");};
                    }
                    input.Enabled=row.Options.Any(o=>o.Enabled);focus.AccessibleName=row.Label;
                    foreach(var o in row.Options)if(o.Command!=0)inputs[o.Command]=focus;
                    bindings.Add(Tuple.Create(row,input));
                    if(row.Kind==2){group.AddFullItem(new LiteActionRow("",input));}
                    else {var item=new LiteSettingsItem(row.Label,input);group.AddFullItem(item);
                        // Keep upstream row/control implementation, adapt width for Pulse's long labels.
                        item.Layout+=(s,e)=>{int gap=UIUtils.S(18);int available=item.Width-input.Width-gap;
                            bool narrow=available<UIUtils.S(190);item.Height=UIUtils.S(narrow?76:44);
                            item.Label.MaximumSize=new Size(Math.Max(40,narrow?item.Width:available),0);
                            item.Label.Location=new Point(0,UIUtils.S(8));input.Location=new Point(narrow?0:item.Width-input.Width,narrow?UIUtils.S(40):UIUtils.S(8));};
                    }
                }
            } finally {Container.ResumeLayout(true);Updating=false;}
        }
        public void Update(Page page) {
            if(!Same(Model,page)){if(dirty.Count!=0)return;Build(page);return;}
            Updating=true;
            try {for(int i=0;i<Model.Rows.Count;i++){var old=Model.Rows[i];var next=page.Rows[i];
                var pair=bindings.FirstOrDefault(b=>Object.ReferenceEquals(b.Item1,old));if(pair==null)continue;var c=pair.Item2;
                c.Enabled=next.Options.Any(o=>o.Enabled);
                if(c is LiteCheck)((LiteCheck)c).Checked=next.Options[0].Checked;
                else if(c is LiteComboBox)((LiteComboBox)c).SelectedIndex=next.Options.FindIndex(o=>o.Checked);
                else if(c is LiteUnderlineInput){var edit=((LiteUnderlineInput)c).Inner;if(edit.Text==next.Options[0].Label)dirty.Remove(c);else if(!dirty.Contains(c))edit.Text=next.Options[0].Label;}
                for(int j=0;j<old.Options.Count;j++){old.Options[j].Label=next.Options[j].Label;old.Options[j].Checked=next.Options[j].Checked;old.Options[j].Enabled=next.Options[j].Enabled;}
            }}finally{Updating=false;}
        }
        public void CommitText(){foreach(var pair in bindings)if(pair.Item2 is LiteUnderlineInput&&dirty.Contains(pair.Item2)){var text=((LiteUnderlineInput)pair.Item2).Inner.Text;if(text!=pair.Item1.Options[0].Label)dispatch(pair.Item1.Options[0].Command,text);}}
        public IntPtr Find(int command){Control c;return inputs.TryGetValue(command,out c)?c.Handle:IntPtr.Zero;}
        public bool CheckScrolling() {
            var before=inputs.ToDictionary(p=>p.Key,p=>p.Value.Bounds);var old=Container.AutoScrollPosition;
            bool moved=false;
            for(int i=0;i<30;i++){Wire.SendMessage(Container.Handle,0x115,(IntPtr)(i%2==0?7:6),IntPtr.Zero);Container.Refresh();moved|=Container.AutoScrollPosition.Y!=0;}
            Container.AutoScrollPosition=new Point(-old.X,-old.Y);
            return (!Container.VerticalScroll.Visible||moved)&&inputs.All(p=>p.Value.Bounds==before[p.Key]);
        }
    }
    sealed class SettingsForm : Form {
        readonly IntPtr parent;readonly uint parentPid;readonly bool test;
        readonly FlowLayoutPanel nav;readonly BufferedPanel content;readonly Label status;
        readonly Dictionary<int,SettingsPage> pages=new Dictionary<int,SettingsPage>();
        int current=-1;bool dark,initialized;readonly Timer lifetime;
        protected override CreateParams CreateParams {get{var cp=base.CreateParams;cp.ExStyle|=0x02000000;return cp;}}
        protected override void OnResizeBegin(EventArgs e){SuspendLayout();base.OnResizeBegin(e);}
        protected override void OnResizeEnd(EventArgs e){base.OnResizeEnd(e);ResumeLayout(true);}
        public SettingsForm(IntPtr owner,uint pid,bool testing) {
            parent=owner;parentPid=pid;test=testing;UIUtils.ScaleFactor=Math.Max(96,Wire.GetDpiForWindow(Handle))/96f;
            var area=Screen.FromHandle(Handle).WorkingArea;
            Size=new Size(Math.Min(area.Width,UIUtils.S(920)),Math.Min(area.Height,UIUtils.S(720)));MinimumSize=new Size(UIUtils.S(700),UIUtils.S(460));
            StartPosition=FormStartPosition.CenterScreen;Text="Pulse";AutoScaleMode=AutoScaleMode.None;Font=new Font("Microsoft YaHei UI",9F);BackColor=UIColors.MainBg;
            var sidebar=new Panel{Dock=DockStyle.Left,Width=UIUtils.S(170),BackColor=UIColors.SidebarBg};
            nav=new FlowLayoutPanel{Dock=DockStyle.Fill,FlowDirection=FlowDirection.TopDown,WrapContents=false,AutoScroll=true,Padding=UIUtils.S(new Padding(0,20,0,0)),BackColor=UIColors.SidebarBg};
            sidebar.Controls.Add(nav);sidebar.Controls.Add(new Panel{Dock=DockStyle.Right,Width=1,BackColor=UIColors.Border});Controls.Add(sidebar);
            var bottom=new Panel{Dock=DockStyle.Bottom,Height=UIUtils.S(60),BackColor=UIColors.MainBg};
            bottom.Paint+=(s,e)=>{using(var pen=new Pen(UIColors.Border))e.Graphics.DrawLine(pen,0,0,Width,0);};
            var close=new LiteButton("关闭 / Close",true){Dock=DockStyle.Right,Width=UIUtils.S(120)};close.Click+=(s,e)=>Close();
            bottom.Padding=UIUtils.S(new Padding(16,14,20,14));bottom.Controls.Add(close);
            status=new Label{Dock=DockStyle.Fill,TextAlign=ContentAlignment.MiddleLeft,ForeColor=UIColors.TextSub};bottom.Controls.Add(status);Controls.Add(bottom);
            content=new BufferedPanel{Dock=DockStyle.Fill,Padding=new Padding(0)};Controls.Add(content);sidebar.BringToFront();bottom.SendToBack();content.BringToFront();
            Shown+=(s,e)=>Wire.Send(parent,Handle,2,0,"");FormClosing+=(s,e)=>{foreach(var page in pages.Values)page.CommitText();};
            lifetime=new Timer{Interval=1000};lifetime.Tick+=(s,e)=>{uint actual;Wire.GetWindowThreadProcessId(parent,out actual);if(!Wire.IsWindow(parent)||actual!=parentPid)Close();};lifetime.Start();
        }
        void Dispatch(int command,string text){if(!Wire.Send(parent,Handle,1,command,text))status.Text="无法保存，请重新打开设置 / Save failed; reopen settings";}
        public void SwitchPage(int key) {
            if(current==key||!pages.ContainsKey(key))return;
            if(pages.ContainsKey(current))pages[current].CommitText();current=key;
            nav.SuspendLayout();foreach(Control c in nav.Controls)if(c is LiteNavBtn)((LiteNavBtn)c).IsActive=(int)c.Tag==key;nav.ResumeLayout();nav.Refresh();
            // LiteMonitor page-swap protocol: size before docking, suspended layout.
            content.SuspendLayout();try{content.Controls.Clear();var page=pages[key];page.Size=content.ClientSize;page.Dock=DockStyle.Fill;content.Controls.Add(page);}finally{content.ResumeLayout();}
        }
        static void Theme(bool dark) {
            UIColors.MainBg=dark?Color.FromArgb(28,28,28):Color.FromArgb(243,243,243);UIColors.SidebarBg=dark?Color.FromArgb(32,32,32):Color.FromArgb(240,240,240);
            UIColors.CardBg=dark?Color.FromArgb(40,40,40):Color.White;UIColors.GroupHeader=dark?Color.FromArgb(48,48,48):Color.FromArgb(248,249,250);
            UIColors.Border=dark?Color.FromArgb(64,64,64):Color.FromArgb(220,220,220);UIColors.TextMain=dark?Color.WhiteSmoke:Color.FromArgb(32,32,32);
            UIColors.TextSub=dark?Color.Silver:Color.FromArgb(90,90,90);UIColors.NavSelected=dark?Color.FromArgb(55,55,55):Color.FromArgb(230,230,230);UIColors.NavHover=UIColors.NavSelected;
        }
        void Model(byte[] raw) {
            using(var stream=new MemoryStream(raw))using(var r=new BinaryReader(stream)) {
                int revision=r.ReadInt32();bool nextDark=r.ReadInt32()!=0;string title=Wire.String(r),hint=Wire.String(r);int count=r.ReadInt32();if(count<1||count>32)throw new InvalidDataException();
                var incoming=new List<Page>();for(int p=0;p<count;p++){var page=new Page{Title=Wire.String(r),Description=Wire.String(r)};int rows=r.ReadInt32();if(rows<0||rows>512)throw new InvalidDataException();
                    for(int i=0;i<rows;i++){var row=new Row{Kind=r.ReadInt32(),Label=Wire.String(r)};int options=r.ReadInt32();if(options<0||options>512||row.Kind<0||row.Kind>5)throw new InvalidDataException();
                        for(int j=0;j<options;j++)row.Options.Add(new Option{Command=r.ReadInt32(),Enabled=r.ReadInt32()!=0,Checked=r.ReadInt32()!=0,Label=Wire.String(r)});page.Rows.Add(row);}incoming.Add(page);}
                if(stream.Position!=stream.Length)throw new InvalidDataException();
                bool rebuild=!initialized||dark!=nextDark||pages.Count!=count||incoming.Where((p,i)=>!pages.ContainsKey(i)||pages[i].Model.Title!=p.Title).Any();
                Text=title;status.Text=hint;dark=nextDark;Theme(dark);
                if(rebuild){int selected=Math.Max(0,current);content.Controls.Clear();foreach(var page in pages.Values)page.Dispose();pages.Clear();foreach(Control c in nav.Controls.Cast<Control>().ToArray())c.Dispose();nav.Controls.Clear();
                    BackColor=UIColors.MainBg;nav.BackColor=UIColors.SidebarBg;status.ForeColor=UIColors.TextSub;status.Parent.BackColor=UIColors.MainBg;
                    for(int i=0;i<count;i++){int key=i;pages[i]=new SettingsPage(incoming[i],Dispatch);var button=new LiteNavBtn(incoming[i].Title){Tag=i};button.Click+=(s,e)=>SwitchPage(key);nav.Controls.Add(button);}current=-1;SwitchPage(Math.Min(selected,count-1));
                }else for(int i=0;i<count;i++)pages[i].Update(incoming[i]);
                initialized=true;Wire.Send(parent,Handle,4,revision,"");
            }
        }
        protected override void WndProc(ref Message m) {
            if(m.Msg==Wire.CopyData&&m.WParam==parent) {
                try{var p=(Wire.Packet)Marshal.PtrToStructure(m.LParam,typeof(Wire.Packet));if(p.Tag!=(IntPtr)601||p.Bytes<4||p.Bytes>524288)throw new InvalidDataException();var raw=new byte[p.Bytes];Marshal.Copy(p.Data,raw,0,p.Bytes);Model(raw);m.Result=(IntPtr)1;}
                catch(Exception e){status.Text="设置读取失败 / Settings unavailable: "+e.GetType().Name;m.Result=IntPtr.Zero;}return;
            }
            if(test&&m.Msg==Wire.SelectPage){SwitchPage(m.WParam.ToInt32());m.Result=(IntPtr)1;return;}
            if(test&&m.Msg==Wire.FindControl){m.Result=pages.ContainsKey(current)?pages[current].Find(m.WParam.ToInt32()):IntPtr.Zero;return;}
            if(test&&m.Msg==Wire.Commit){if(pages.ContainsKey(current))pages[current].CommitText();m.Result=(IntPtr)1;return;}
            if(test&&m.Msg==Wire.ScrollCheck){m.Result=pages.ContainsKey(current)&&pages[current].CheckScrolling()?(IntPtr)1:IntPtr.Zero;return;}
            base.WndProc(ref m);
        }
        protected override void Dispose(bool disposing){if(disposing){lifetime.Dispose();foreach(var p in pages.Values)p.Dispose();}base.Dispose(disposing);}
    }
    static class Program {
        [STAThread] static int Main(string[] args) {
            try {
                if(args.Length!=3)return 2;long handle;uint pid;if(!long.TryParse(args[0],out handle)||!uint.TryParse(args[1],out pid))return 2;
                var parent=(IntPtr)handle;uint actual;Wire.GetWindowThreadProcessId(parent,out actual);var name=new StringBuilder(128);Wire.GetClassName(parent,name,128);
                if(actual!=pid||name.ToString()!="Pulse.SettingsBridge.v2")return 2;
                Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);Application.Run(new SettingsForm(parent,pid,args[2]=="test"));return 0;
            }catch{return 1;}
        }
    }
}
