using System.Windows;
using ProjectRebound.Browser.ViewModels;

namespace ProjectRebound.Browser;

public partial class MainWindow : Window
{
    private readonly MainViewModel _viewModel = new();

    public MainWindow()
    {
        InitializeComponent();
        DataContext = _viewModel;
    }

    private async void Window_Loaded(object sender, RoutedEventArgs e)
    {
        try
        {
            await _viewModel.InitializeAsync();
        }
        catch (Exception ex)
        {
            _viewModel.Status = $"Startup failed: {ex.Message}";
        }
    }
}
