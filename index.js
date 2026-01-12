const opcda = require('./build/Release/opcda');

// Init with callback for connection events
const client = new opcda.OPCDA((event) => {
  console.log('Connection event:', event.type, event.data);
  if (event.type === 'init') {
    console.log('OPCDA initialized');
  } else if (event.type === 'connect' && event.data.success) {
    console.log('Connected!');
    client.createGroup('myGroup', 1000, 0.0);
    client.addItem('myGroup', '_System._ProjectTitle');
    // Subscribe to data changes (separate tsfn)
    client.subscribe('myGroup', (event) => {
      if (event.type === 'dataChange') {
        console.log('Value changed:', event.data.data.value);
      } else if (event.type === 'disconnect') {
        console.error('Group disconnect:', event.data.error);
      }
    }, ['dataChange', 'disconnect']);
  } else if (event.type === 'disconnect') {
    console.error('Disconnected:', event.data.error);
    // Auto-reconnect? client.connect('localhost', 'Kepware.KEPServerEX.V6');
  }
});

// Connect (events go to init callback)
client.connect('localhost', 'Kepware.KEPServerEX.V6');

// Example: Browse
client.browse('').then(items => {
  console.log('Browsed items:', items);
});

// Example: Read
client.read('_System._ProjectTitle').then(value => {
  console.log('Read value:', value);
});

// Example: Write
client.write('Some.Item', 42).then(() => {
  console.log('Write OK');
});

// Unsubscribe example
// client.unsubscribe('myGroup', ['dataChange']);
// client.unsubscribeConnection(); // For init tsfn

// Graceful shutdown
process.on('SIGINT', () => {
  client.disconnect();
  client.unsubscribeConnection();
  process.exit(0);
});

console.log('OPC DA client started');