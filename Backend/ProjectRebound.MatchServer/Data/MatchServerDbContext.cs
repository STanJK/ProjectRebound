using Microsoft.EntityFrameworkCore;
using Microsoft.EntityFrameworkCore.Storage.ValueConversion;
using ProjectRebound.Contracts;

namespace ProjectRebound.MatchServer.Data;

public sealed class MatchServerDbContext(DbContextOptions<MatchServerDbContext> options) : DbContext(options)
{
    public DbSet<Player> Players => Set<Player>();
    public DbSet<HostProbe> HostProbes => Set<HostProbe>();
    public DbSet<Room> Rooms => Set<Room>();
    public DbSet<RoomPlayer> RoomPlayers => Set<RoomPlayer>();
    public DbSet<MatchTicket> MatchTickets => Set<MatchTicket>();
    public DbSet<LegacyServer> LegacyServers => Set<LegacyServer>();

    protected override void OnModelCreating(ModelBuilder modelBuilder)
    {
        var dateTimeOffsetConverter = new ValueConverter<DateTimeOffset, long>(
            value => value.ToUnixTimeMilliseconds(),
            value => DateTimeOffset.FromUnixTimeMilliseconds(value));

        modelBuilder.Entity<Player>(entity =>
        {
            entity.HasKey(x => x.PlayerId);
            entity.HasIndex(x => x.DeviceTokenHash).IsUnique();
            entity.Property(x => x.Status).HasConversion<string>();
        });

        modelBuilder.Entity<HostProbe>(entity =>
        {
            entity.HasKey(x => x.ProbeId);
            entity.HasIndex(x => new { x.PlayerId, x.Status });
            entity.Property(x => x.Status).HasConversion<string>();
        });

        modelBuilder.Entity<Room>(entity =>
        {
            entity.HasKey(x => x.RoomId);
            entity.HasIndex(x => new { x.State, x.Region, x.Version });
            entity.HasIndex(x => x.LastSeenAt);
            entity.Property(x => x.State).HasConversion<string>();
        });

        modelBuilder.Entity<RoomPlayer>(entity =>
        {
            entity.HasKey(x => x.RoomPlayerId);
            entity.HasIndex(x => new { x.RoomId, x.PlayerId, x.Status });
            entity.HasIndex(x => x.ExpiresAt);
            entity.Property(x => x.Status).HasConversion<string>();
        });

        modelBuilder.Entity<MatchTicket>(entity =>
        {
            entity.HasKey(x => x.TicketId);
            entity.HasIndex(x => new { x.State, x.Region, x.Version });
            entity.HasIndex(x => x.ExpiresAt);
            entity.Property(x => x.State).HasConversion<string>();
        });

        modelBuilder.Entity<LegacyServer>(entity =>
        {
            entity.HasKey(x => x.ServerId);
            entity.HasIndex(x => x.SourceKey).IsUnique();
            entity.HasIndex(x => x.LastSeenAt);
        });

        foreach (var entityType in modelBuilder.Model.GetEntityTypes())
        {
            foreach (var property in entityType.ClrType.GetProperties().Where(x => x.PropertyType == typeof(DateTimeOffset)))
            {
                modelBuilder.Entity(entityType.ClrType)
                    .Property(property.Name)
                    .HasConversion(dateTimeOffsetConverter)
                    .HasColumnType("INTEGER");
            }
        }
    }
}
