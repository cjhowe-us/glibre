namespace Editor.ViewModels;

public partial class MainWindowViewModel : ViewModelBase
{
    public string AppName { get; } = "glibre";
    public string ProjectName { get; } = "Untitled";
    public string WindowTitle => $"{AppName} — {ProjectName}";
}
